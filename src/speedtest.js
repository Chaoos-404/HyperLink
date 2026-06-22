import net from 'node:net';
import { once } from 'node:events';
import { DEFAULT_PORT, DEFAULT_SOCKET_HIGH_WATER_MARK } from './protocol.js';

const DEFAULT_SPEED_PORT = DEFAULT_PORT + 1;
const DEFAULT_SPEED_BYTES = 512 * 1024 * 1024;
const DEFAULT_SPEED_CHUNK_SIZE = 4 * 1024 * 1024;
const TCP_PROBE = Buffer.from('HLINK_TCP_SPEED_PROBE_v1');
const TCP_ACK = Buffer.from('HLINK_TCP_SPEED_ACK_v1');

export async function startSpeedServer({
  host,
  port = DEFAULT_SPEED_PORT,
  socketHighWaterMark = DEFAULT_SOCKET_HIGH_WATER_MARK,
  diagnosticsIntervalMs = 1000,
  onEvent = () => {}
} = {}) {
  const server = net.createServer({ highWaterMark: socketHighWaterMark }, (socket) => {
    socket.setNoDelay(true);
    socket.setKeepAlive(true, 5000);
    onEvent(socketEvent({ side: 'server', socket }));

    const startedAt = performance.now();
    let lastReportAt = startedAt;
    let bytes = 0;
    let firstChunk = true;

    socket.on('data', (chunk) => {
      if (firstChunk && chunk.equals(TCP_PROBE)) {
        socket.end(TCP_ACK);
        return;
      }
      firstChunk = false;
      bytes += chunk.length;
      const now = performance.now();
      if (now - lastReportAt >= diagnosticsIntervalMs) {
        lastReportAt = now;
        onEvent(speedEvent({ side: 'server', final: false, startedAt, bytes }));
      }
    });

    socket.on('end', () => {
      onEvent(speedEvent({ side: 'server', final: true, startedAt, bytes }));
    });
  });

  await new Promise((resolve, reject) => {
    server.once('error', reject);
    server.listen(port, host, () => {
      server.off('error', reject);
      resolve();
    });
  });

  return {
    server,
    port: server.address().port,
    close: () => new Promise((resolve) => server.close(resolve))
  };
}

export async function runSpeedClient({
  host,
  port = DEFAULT_SPEED_PORT,
  autoPort = false,
  portRange,
  portProbeTimeoutMs = 250,
  bytes = DEFAULT_SPEED_BYTES,
  chunkSize = DEFAULT_SPEED_CHUNK_SIZE,
  socketHighWaterMark = DEFAULT_SOCKET_HIGH_WATER_MARK,
  diagnosticsIntervalMs = 1000,
  onEvent = () => {}
} = {}) {
  if (!host) throw new Error('speed-client requires --host');

  if (autoPort) {
    port = await detectTcpSpeedPort({ host, ports: portCandidates({ port, portRange }), timeoutMs: portProbeTimeoutMs });
    onEvent({ type: 'speed_port_detected', host, port });
  }

  const socket = await connect(host, port, socketHighWaterMark);
  onEvent(socketEvent({ side: 'client', socket }));
  const chunk = Buffer.alloc(chunkSize);
  const startedAt = performance.now();
  let lastReportAt = startedAt;
  let sent = 0;
  let writeMs = 0;

  while (sent < bytes) {
    const size = Math.min(chunk.length, bytes - sent);
    const payload = size === chunk.length ? chunk : chunk.subarray(0, size);
    const writeStarted = performance.now();
    if (!socket.write(payload)) await once(socket, 'drain');
    writeMs += performance.now() - writeStarted;
    sent += size;

    const now = performance.now();
    if (now - lastReportAt >= diagnosticsIntervalMs) {
      lastReportAt = now;
      onEvent(speedEvent({ side: 'client', final: false, startedAt, bytes: sent, writeMs, chunkSize, port }));
    }
  }

  await new Promise((resolve) => socket.end(resolve));
  onEvent(speedEvent({ side: 'client', final: true, startedAt, bytes: sent, writeMs, chunkSize, port }));
}

export function defaultSpeedPort() {
  return DEFAULT_SPEED_PORT;
}

export function defaultSpeedBytes() {
  return DEFAULT_SPEED_BYTES;
}

export function defaultSpeedChunkSize() {
  return DEFAULT_SPEED_CHUNK_SIZE;
}

function speedEvent({ side, final, startedAt, bytes, writeMs = 0, chunkSize = DEFAULT_SPEED_CHUNK_SIZE, port }) {
  const elapsedSeconds = Math.max((performance.now() - startedAt) / 1000, 0.001);
  return {
    type: 'speed',
    side,
    final,
    bytes,
    elapsedSeconds,
    throughputBytesPerSecond: bytes / elapsedSeconds,
    writeMs,
    chunkSize,
    port
  };
}

function socketEvent({ side, socket }) {
  return {
    type: 'speed_peer',
    side,
    localAddress: socket.localAddress,
    localPort: socket.localPort,
    remoteAddress: socket.remoteAddress,
    remotePort: socket.remotePort,
    readableHighWaterMark: socket.readableHighWaterMark,
    writableHighWaterMark: socket.writableHighWaterMark
  };
}

async function connect(host, port, socketHighWaterMark) {
  return await new Promise((resolve, reject) => {
    const socket = net.connect({ host, port, highWaterMark: socketHighWaterMark });
    socket.once('connect', () => {
      socket.setNoDelay(true);
      socket.setKeepAlive(true, 5000);
      resolve(socket);
    });
    socket.once('error', reject);
  });
}

async function detectTcpSpeedPort({ host, ports, timeoutMs }) {
  for (const port of ports) {
    const matched = await probeTcpSpeedPort({ host, port, timeoutMs });
    if (!matched) continue;
    return port;
  }

  throw new Error(`No TCP speed server found on ${host} ports ${ports.join(', ')}`);
}

async function probeTcpSpeedPort({ host, port, timeoutMs }) {
  return await new Promise((resolve) => {
    let settled = false;
    const socket = new net.Socket();
    const timer = setTimeout(() => done(null), timeoutMs);

    const done = (result) => {
      if (settled) return;
      settled = true;
      clearTimeout(timer);
      socket.off('connect', onConnect);
      socket.off('error', onError);
      socket.off('data', onData);
      socket.destroy();
      resolve(result);
    };

    const onConnect = () => socket.write(TCP_PROBE);
    const onError = () => done(null);
    const onData = (chunk) => done(chunk.equals(TCP_ACK));

    socket.once('connect', onConnect);
    socket.once('error', onError);
    socket.once('data', onData);
    socket.connect({ host, port });
  });
}

function portCandidates({ port, portRange }) {
  if (Array.isArray(portRange) && portRange.length) return uniquePorts(portRange);
  const start = Number(port);
  return uniquePorts(Array.from({ length: 21 }, (_, index) => start + index));
}

function uniquePorts(ports) {
  return [...new Set(ports.map(Number).filter((port) => Number.isInteger(port) && port > 0 && port <= 65535))];
}
