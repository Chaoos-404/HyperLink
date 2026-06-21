import net from 'node:net';
import { once } from 'node:events';
import { DEFAULT_PORT, DEFAULT_SOCKET_HIGH_WATER_MARK } from './protocol.js';

const DEFAULT_SPEED_PORT = DEFAULT_PORT + 1;
const DEFAULT_SPEED_BYTES = 512 * 1024 * 1024;
const DEFAULT_SPEED_CHUNK_SIZE = 4 * 1024 * 1024;

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

    socket.on('data', (chunk) => {
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
  bytes = DEFAULT_SPEED_BYTES,
  chunkSize = DEFAULT_SPEED_CHUNK_SIZE,
  socketHighWaterMark = DEFAULT_SOCKET_HIGH_WATER_MARK,
  diagnosticsIntervalMs = 1000,
  onEvent = () => {}
} = {}) {
  if (!host) throw new Error('speed-client requires --host');

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
      onEvent(speedEvent({ side: 'client', final: false, startedAt, bytes: sent, writeMs, chunkSize }));
    }
  }

  await new Promise((resolve) => socket.end(resolve));
  onEvent(speedEvent({ side: 'client', final: true, startedAt, bytes: sent, writeMs, chunkSize }));
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

function speedEvent({ side, final, startedAt, bytes, writeMs = 0, chunkSize = DEFAULT_SPEED_CHUNK_SIZE }) {
  const elapsedSeconds = Math.max((performance.now() - startedAt) / 1000, 0.001);
  return {
    type: 'speed',
    side,
    final,
    bytes,
    elapsedSeconds,
    throughputBytesPerSecond: bytes / elapsedSeconds,
    writeMs,
    chunkSize
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
