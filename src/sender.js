import fsp from 'node:fs/promises';
import fs from 'node:fs';
import net from 'node:net';
import path from 'node:path';
import { DEFAULT_BLOCK_SIZE, DEFAULT_PIPELINE_WINDOW, DEFAULT_SOCKET_HIGH_WATER_MARK, FrameReader, LEGACY_BLOCK_SIZE, PROTOCOL_VERSION, writeFrame } from './protocol.js';
import { FALLBACK_HASH_ALGORITHM, SUPPORTED_HASH_ALGORITHMS, createFastHash, hashBuffer, initHashing } from './hash.js';
import { discoverPeers } from './discovery.js';

export async function sendPaths({
  paths,
  host,
  port = 44777,
  bind,
  token,
  blockSize = DEFAULT_BLOCK_SIZE,
  pipelineWindow = DEFAULT_PIPELINE_WINDOW,
  retries = 2,
  diagnostics = false,
  diagnosticsIntervalMs = 1000,
  socketHighWaterMark,
  onEvent = () => {}
}) {
  await initHashing();
  if (!paths?.length) throw new Error('No input paths were provided');
  const peer = host ? { host, port } : await chooseDiscoveredPeer();
  const requestedHighWaterMark = socketHighWaterMark ?? defaultSocketHighWaterMark({ blockSize, pipelineWindow });
  const socket = await connect(peer.host, peer.port ?? port, bind, requestedHighWaterMark);
  const reader = new FrameReader(socket);

  await writeFrame(socket, {
    type: 'hello',
    version: PROTOCOL_VERSION,
    agent: 'hyperlink-node/0.1.0',
    supportedHashAlgorithms: SUPPORTED_HASH_ALGORITHMS,
    token,
    startedAt: new Date().toISOString()
  });
  const hello = await expect(reader, 'hello_ack');
  const transferProfile = negotiateTransferProfile(hello, { blockSize, pipelineWindow });
  onEvent({ type: 'negotiated', ...transferProfile, peer: hello, ...socketWatermarks(socket) });

  for (const source of paths) {
    for await (const entry of walkSource(source)) {
      if (entry.type === 'dir') {
        await writeFrame(socket, { type: 'dir', path: entry.relativePath });
        await expect(reader, 'dir_ack');
        onEvent({ type: 'dir', path: entry.relativePath });
      } else if (entry.type === 'file') {
        await sendFile(socket, reader, entry, {
          ...transferProfile,
          retries,
          diagnostics,
          diagnosticsIntervalMs,
          onEvent
        });
      }
    }
  }

  await writeFrame(socket, { type: 'done' });
  await expect(reader, 'done_ack');
  socket.end();
}

export async function sendVirtualFile({
  stream,
  relativePath,
  size,
  host,
  port = 44777,
  token,
  blockSize = DEFAULT_BLOCK_SIZE,
  pipelineWindow = DEFAULT_PIPELINE_WINDOW,
  diagnostics = false,
  diagnosticsIntervalMs = 1000,
  socketHighWaterMark,
  onEvent = () => {}
}) {
  await initHashing();
  const requestedHighWaterMark = socketHighWaterMark ?? defaultSocketHighWaterMark({ blockSize, pipelineWindow });
  const socket = await connect(host, port, undefined, requestedHighWaterMark);
  const reader = new FrameReader(socket);
  await writeFrame(socket, {
    type: 'hello',
    version: PROTOCOL_VERSION,
    agent: 'hyperlink-gui/0.1.0',
    supportedHashAlgorithms: SUPPORTED_HASH_ALGORITHMS,
    token
  });
  const hello = await expect(reader, 'hello_ack');
  const transferProfile = negotiateTransferProfile(hello, { blockSize, pipelineWindow });
  onEvent({ type: 'negotiated', ...transferProfile, peer: hello, ...socketWatermarks(socket) });
  await sendStream(socket, reader, {
    relativePath,
    size,
    mode: 0o644,
    mtimeMs: Date.now(),
    stream
  }, { ...transferProfile, retries: 2, diagnostics, diagnosticsIntervalMs, onEvent });
  await writeFrame(socket, { type: 'done' });
  await expect(reader, 'done_ack');
  socket.end();
}

async function sendFile(socket, reader, entry, options) {
  const stream = fs.createReadStream(entry.absolutePath, { highWaterMark: options.blockSize });
  await sendStream(socket, reader, { ...entry, stream }, options);
}

async function sendStream(socket, reader, entry, {
  blockSize,
  pipelineWindow,
  retries,
  hashAlgorithm,
  diagnostics,
  diagnosticsIntervalMs,
  onEvent
}) {
  await writeFrame(socket, {
    type: 'file',
    path: entry.relativePath,
    size: entry.size,
    mode: entry.mode,
    mtimeMs: entry.mtimeMs,
    blockSize,
    hashAlgorithm
  });
  await expect(reader, 'file_ready');
  onEvent({ type: 'file_start', path: entry.relativePath, size: entry.size });

  const fileHash = createFastHash(hashAlgorithm);
  let index = 0;
  let bytes = 0;
  let pending = 0;
  let nextAck = 0;
  const pendingSizes = new Map();
  const windowSize = Math.max(1, pipelineWindow);
  const stats = {
    startedAt: performance.now(),
    lastReportAt: performance.now(),
    blocksWritten: 0,
    blocksAcked: 0,
    bytesWritten: 0,
    bytesAcked: 0,
    hashMs: 0,
    writeMs: 0,
    ackWaitMs: 0
  };

  for await (const chunk of entry.stream) {
    const payload = Buffer.isBuffer(chunk) ? chunk : Buffer.from(chunk);
    const hashStarted = performance.now();
    fileHash.update(payload);
    const blockHeader = {
      path: entry.relativePath,
      index,
      hash: hashBuffer(payload, hashAlgorithm),
      size: payload.length
    };
    stats.hashMs += performance.now() - hashStarted;

    if (windowSize === 1) {
      await sendBlockWithRetry(socket, reader, blockHeader, payload, retries, stats);
      stats.bytesAcked += payload.length;
      stats.blocksAcked += 1;
      onEvent({ type: 'bytes', path: entry.relativePath, bytes: payload.length });
    } else {
      const writeStarted = performance.now();
      await writeFrame(socket, { type: 'block', ...blockHeader }, payload);
      stats.writeMs += performance.now() - writeStarted;
      stats.bytesWritten += payload.length;
      stats.blocksWritten += 1;
      pending += 1;
      pendingSizes.set(index, payload.length);
    }

    bytes += payload.length;
    index += 1;

    while (pending >= windowSize) {
      const ackStarted = performance.now();
      const ack = await expectBlockAck(reader, entry.relativePath, nextAck);
      stats.ackWaitMs += performance.now() - ackStarted;
      pending -= 1;
      nextAck += 1;
      const ackedBytes = pendingSizes.get(ack.index) ?? 0;
      stats.bytesAcked += ackedBytes;
      stats.blocksAcked += 1;
      onEvent({ type: 'bytes', path: entry.relativePath, bytes: ackedBytes });
      pendingSizes.delete(ack.index);
    }

    maybeReportDiagnostics({ diagnostics, diagnosticsIntervalMs, stats, path: entry.relativePath, onEvent });
  }

  while (pending > 0) {
    const ackStarted = performance.now();
    const ack = await expectBlockAck(reader, entry.relativePath, nextAck);
    stats.ackWaitMs += performance.now() - ackStarted;
    pending -= 1;
    nextAck += 1;
    const ackedBytes = pendingSizes.get(ack.index) ?? 0;
    stats.bytesAcked += ackedBytes;
    stats.blocksAcked += 1;
    onEvent({ type: 'bytes', path: entry.relativePath, bytes: ackedBytes });
    pendingSizes.delete(ack.index);
    maybeReportDiagnostics({ diagnostics, diagnosticsIntervalMs, stats, path: entry.relativePath, onEvent });
  }

  const digest = fileHash.digest('hex');
  await writeFrame(socket, { type: 'file_end', path: entry.relativePath, size: bytes, hash: digest });
  await expect(reader, 'file_end_ack');
  onEvent({ type: 'file_done', path: entry.relativePath, bytes });
  emitDiagnostics({ stats, path: entry.relativePath, final: true, onEvent });
}

async function sendBlockWithRetry(socket, reader, header, payload, retries, stats = null) {
  for (let attempt = 0; attempt <= retries; attempt += 1) {
    const writeStarted = performance.now();
    await writeFrame(socket, { type: 'block', ...header }, payload);
    if (stats) {
      stats.writeMs += performance.now() - writeStarted;
      stats.bytesWritten += payload.length;
      stats.blocksWritten += 1;
    }
    const ackStarted = performance.now();
    const response = await reader.readFrame();
    if (stats) stats.ackWaitMs += performance.now() - ackStarted;
    if (!response) throw new Error('Peer disconnected during block transfer');
    if (response.header.type === 'block_ack') return;
    if (response.header.type === 'error') throw new Error(response.header.message);
    if (response.header.type !== 'block_nack') {
      throw new Error(`Unexpected response while sending block: ${response.header.type}`);
    }
  }
  throw new Error(`Block ${header.index} failed after ${retries + 1} attempts`);
}

function negotiateTransferProfile(hello, { blockSize, pipelineWindow }) {
  const hashAlgorithm = hello.hashAlgorithm ?? FALLBACK_HASH_ALGORITHM;
  const peerMaxBlockSize = hello.maxBlockSize ?? LEGACY_BLOCK_SIZE;
  return {
    hashAlgorithm,
    blockSize: Math.min(blockSize, peerMaxBlockSize),
    pipelineWindow: hello.supportsPipelining ? pipelineWindow : 1,
    legacyPeer: !hello.maxBlockSize,
    supportsPipelining: Boolean(hello.supportsPipelining),
    peerMaxBlockSize
  };
}

function maybeReportDiagnostics({ diagnostics, diagnosticsIntervalMs, stats, path, onEvent }) {
  if (!diagnostics) return;
  const now = performance.now();
  if (now - stats.lastReportAt < diagnosticsIntervalMs) return;
  stats.lastReportAt = now;
  emitDiagnostics({ stats, path, final: false, onEvent });
}

function emitDiagnostics({ stats, path, final, onEvent }) {
  const elapsedSeconds = Math.max((performance.now() - stats.startedAt) / 1000, 0.001);
  onEvent({
    type: 'diagnostic',
    path,
    final,
    elapsedSeconds,
    blocksWritten: stats.blocksWritten,
    blocksAcked: stats.blocksAcked,
    bytesWritten: stats.bytesWritten,
    bytesAcked: stats.bytesAcked,
    throughputBytesPerSecond: stats.bytesAcked / elapsedSeconds,
    hashMs: stats.hashMs,
    writeMs: stats.writeMs,
    ackWaitMs: stats.ackWaitMs
  });
}

async function expectBlockAck(reader, path, index) {
  const response = await reader.readFrame();
  if (!response) throw new Error('Peer disconnected during block transfer');
  if (response.header.type === 'error') throw new Error(response.header.message);
  if (response.header.type === 'block_nack') {
    throw new Error(`Peer rejected ${path} block ${response.header.index}: ${response.header.message}`);
  }
  if (response.header.type !== 'block_ack') {
    throw new Error(`Unexpected response while sending block: ${response.header.type}`);
  }
  if (response.header.index !== index) {
    throw new Error(`Out-of-order block acknowledgement for ${path}: expected ${index}, received ${response.header.index}`);
  }
  return response.header;
}

async function expect(reader, type) {
  const frame = await reader.readFrame();
  if (!frame) throw new Error(`Peer disconnected before ${type}`);
  if (frame.header.type === 'error') throw new Error(frame.header.message);
  if (frame.header.type !== type) throw new Error(`Expected ${type}, received ${frame.header.type}`);
  return frame.header;
}

function defaultSocketHighWaterMark({ blockSize, pipelineWindow }) {
  return Math.max(DEFAULT_SOCKET_HIGH_WATER_MARK, blockSize * Math.max(1, pipelineWindow));
}

function socketWatermarks(socket) {
  return {
    socketReadableHighWaterMark: socket.readableHighWaterMark,
    socketWritableHighWaterMark: socket.writableHighWaterMark
  };
}

async function connect(host, port, bind, socketHighWaterMark) {
  return await new Promise((resolve, reject) => {
    const options = { host, port, highWaterMark: socketHighWaterMark };
    if (bind) options.localAddress = bind;
    const socket = net.connect(options);
    socket.once('connect', () => {
      socket.setNoDelay(true);
      socket.setKeepAlive(true, 5000);
      resolve(socket);
    });
    socket.once('error', reject);
  });
}

async function chooseDiscoveredPeer() {
  const peers = await discoverPeers({ timeoutMs: 2500 });
  if (!peers.length) throw new Error('No BoltBridge peers discovered. Pass --host explicitly.');
  return peers[0];
}

async function* walkSource(source) {
  const absolute = path.resolve(source);
  const stat = await fsp.stat(absolute);
  const root = path.dirname(absolute);
  yield* walkEntry(absolute, root, stat);
}

async function* walkEntry(absolute, root, stat = null) {
  const current = stat ?? await fsp.stat(absolute);
  const relativePath = path.relative(root, absolute).replaceAll(path.sep, '/');

  if (current.isDirectory()) {
    yield { type: 'dir', absolutePath: absolute, relativePath };
    const children = await fsp.readdir(absolute);
    for (const child of children) {
      yield* walkEntry(path.join(absolute, child), root);
    }
    return;
  }

  if (current.isFile()) {
    yield {
      type: 'file',
      absolutePath: absolute,
      relativePath,
      size: current.size,
      mode: current.mode,
      mtimeMs: current.mtimeMs
    };
  }
}
