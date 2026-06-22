import dgram from 'node:dgram';
import { DEFAULT_PORT } from './protocol.js';

const DEFAULT_UDP_SPEED_PORT = DEFAULT_PORT + 2;
const DEFAULT_UDP_SPEED_BYTES = 512 * 1024 * 1024;
const DEFAULT_UDP_PACKET_SIZE = 1200;
const UDP_PROBE = Buffer.from('HLINK_UDP_SPEED_PROBE_v1');
const UDP_ACK = Buffer.from('HLINK_UDP_SPEED_ACK_v1');
const UDP_DONE = Buffer.from('HLINK_UDP_SPEED_DONE_v1');
const UDP_FINAL_PREFIX = 'HLINK_UDP_SPEED_FINAL_v1 ';

export async function startUdpSpeedServer({
  host,
  port = DEFAULT_UDP_SPEED_PORT,
  diagnosticsIntervalMs = 1000,
  onEvent = () => {}
} = {}) {
  const socket = dgram.createSocket(host?.includes(':') ? 'udp6' : 'udp4');
  let startedAt = null;
  let lastReportAt = null;
  let bytes = 0;
  let packets = 0;
  let peerKey = null;

  socket.on('message', (message, remote) => {
    if (message.equals(UDP_PROBE)) {
      socket.send(UDP_ACK, remote.port, remote.address);
      return;
    }

    if (message.equals(UDP_DONE)) {
      let finalEvent = null;
      if (startedAt) {
        finalEvent = udpSpeedEvent({ side: 'server', final: true, startedAt, bytes, packets, packetSize: 0, port });
        onEvent(finalEvent);
      }
      socket.send(udpFinalAck(finalEvent), remote.port, remote.address);
      startedAt = null;
      lastReportAt = null;
      bytes = 0;
      packets = 0;
      peerKey = null;
      return;
    }

    const currentPeerKey = `${remote.address}:${remote.port}`;
    if (!startedAt || peerKey !== currentPeerKey) {
      startedAt = performance.now();
      lastReportAt = startedAt;
      bytes = 0;
      packets = 0;
      peerKey = currentPeerKey;
      onEvent({
        type: 'udp_peer',
        side: 'server',
        localAddress: socket.address().address,
        localPort: socket.address().port,
        remoteAddress: remote.address,
        remotePort: remote.port
      });
    }

    bytes += message.length;
    packets += 1;
    const now = performance.now();
    if (now - lastReportAt >= diagnosticsIntervalMs) {
      lastReportAt = now;
      onEvent(udpSpeedEvent({ side: 'server', final: false, startedAt, bytes, packets, packetSize: message.length, port }));
    }
  });

  await new Promise((resolve, reject) => {
    socket.once('error', reject);
    socket.bind(port, host, () => {
      socket.off('error', reject);
      resolve();
    });
  });

  return {
    socket,
    port: socket.address().port,
    close: () => new Promise((resolve) => socket.close(resolve))
  };
}

export async function runUdpSpeedClient({
  host,
  port = DEFAULT_UDP_SPEED_PORT,
  autoPort = false,
  portRange,
  portProbeTimeoutMs = 250,
  bytes = DEFAULT_UDP_SPEED_BYTES,
  packetSize = DEFAULT_UDP_PACKET_SIZE,
  diagnosticsIntervalMs = 1000,
  finalDelayMs = 25,
  onEvent = () => {}
} = {}) {
  if (!host) throw new Error('udp-speed-client requires --host');

  if (autoPort) {
    port = await detectUdpSpeedPort({ host, ports: portCandidates({ port, portRange }), timeoutMs: portProbeTimeoutMs });
    onEvent({ type: 'udp_port_detected', host, port });
  }

  const socket = dgram.createSocket(host.includes(':') ? 'udp6' : 'udp4');
  const payload = Buffer.alloc(packetSize);
  const startedAt = performance.now();
  let lastReportAt = startedAt;
  let sent = 0;
  let packets = 0;
  let sendMs = 0;

  await new Promise((resolve) => socket.connect(port, host, resolve));
  onEvent({
    type: 'udp_peer',
    side: 'client',
    localAddress: socket.address().address,
    localPort: socket.address().port,
    remoteAddress: host,
    remotePort: port
  });

  while (sent < bytes) {
    const size = Math.min(payload.length, bytes - sent);
    const packet = size === payload.length ? payload : payload.subarray(0, size);
    const sendStarted = performance.now();
    await sendConnected(socket, packet);
    sendMs += performance.now() - sendStarted;
    sent += size;
    packets += 1;

    const now = performance.now();
    if (now - lastReportAt >= diagnosticsIntervalMs) {
      lastReportAt = now;
      onEvent(udpSpeedEvent({ side: 'client', final: false, startedAt, bytes: sent, packets, packetSize, sendMs, port }));
    }
  }

  if (finalDelayMs > 0) {
    await new Promise((resolve) => setTimeout(resolve, finalDelayMs));
  }
  await sendConnected(socket, UDP_DONE);
  const remoteFinal = await waitForUdpFinal(socket, portProbeTimeoutMs);
  socket.close();
  onEvent(udpSpeedEvent({ side: 'client', final: true, startedAt, bytes: sent, packets, packetSize, sendMs, port, remoteFinal }));
}

export function defaultUdpSpeedPort() {
  return DEFAULT_UDP_SPEED_PORT;
}

export function defaultUdpSpeedBytes() {
  return DEFAULT_UDP_SPEED_BYTES;
}

export function defaultUdpPacketSize() {
  return DEFAULT_UDP_PACKET_SIZE;
}

function udpSpeedEvent({ side, final, startedAt, bytes, packets, packetSize, sendMs = 0, port, remoteFinal }) {
  const elapsedSeconds = Math.max((performance.now() - startedAt) / 1000, 0.001);
  const event = {
    type: 'udp_speed',
    side,
    final,
    bytes,
    packets,
    packetSize,
    elapsedSeconds,
    throughputBytesPerSecond: bytes / elapsedSeconds,
    sendMs,
    port
  };

  if (remoteFinal) {
    event.remoteBytes = remoteFinal.bytes;
    event.remotePackets = remoteFinal.packets;
    event.remoteElapsedSeconds = remoteFinal.elapsedSeconds;
    event.lossPercent = bytes > 0 ? ((bytes - remoteFinal.bytes) / bytes) * 100 : 0;
    event.remoteThroughputBytesPerSecond = remoteFinal.bytes / Math.max(remoteFinal.elapsedSeconds, 0.001);
  }

  return event;
}

async function detectUdpSpeedPort({ host, ports, timeoutMs }) {
  for (const port of ports) {
    if (await probeUdpPort({ host, port, timeoutMs })) return port;
  }

  throw new Error(`No UDP speed server found on ${host} ports ${ports.join(', ')}`);
}

async function probeUdpPort({ host, port, timeoutMs }) {
  const socket = dgram.createSocket(host.includes(':') ? 'udp6' : 'udp4');
  try {
    return await new Promise((resolve) => {
      const timer = setTimeout(() => resolve(false), timeoutMs);
      socket.once('message', (message) => {
        clearTimeout(timer);
        resolve(message.equals(UDP_ACK));
      });
      socket.send(UDP_PROBE, port, host, (error) => {
        if (error) {
          clearTimeout(timer);
          resolve(false);
        }
      });
    });
  } finally {
    socket.close();
  }
}

async function sendConnected(socket, payload) {
  await new Promise((resolve, reject) => {
    socket.send(payload, (error) => {
      if (error) reject(error);
      else resolve();
    });
  });
}

async function waitForUdpFinal(socket, timeoutMs) {
  return await new Promise((resolve) => {
    const timer = setTimeout(resolve, timeoutMs);
    socket.once('message', (message) => {
      clearTimeout(timer);
      resolve(parseUdpFinalAck(message));
    });
  });
}

function udpFinalAck(finalEvent) {
  if (!finalEvent) return UDP_ACK;
  return Buffer.from(`${UDP_FINAL_PREFIX}${JSON.stringify({
    bytes: finalEvent.bytes,
    packets: finalEvent.packets,
    elapsedSeconds: finalEvent.elapsedSeconds
  })}`);
}

function parseUdpFinalAck(message) {
  const text = message.toString();
  if (!text.startsWith(UDP_FINAL_PREFIX)) return null;
  try {
    return JSON.parse(text.slice(UDP_FINAL_PREFIX.length));
  } catch {
    return null;
  }
}

function portCandidates({ port, portRange }) {
  if (Array.isArray(portRange) && portRange.length) return uniquePorts(portRange);
  const start = Number(port);
  return uniquePorts(Array.from({ length: 21 }, (_, index) => start + index));
}

function uniquePorts(ports) {
  return [...new Set(ports.map(Number).filter((port) => Number.isInteger(port) && port > 0 && port <= 65535))];
}
