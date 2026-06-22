import assert from 'node:assert/strict';
import test from 'node:test';
import { runUdpSpeedClient, startUdpSpeedServer } from '../src/udpspeed.js';

test('udp speed test sends datagrams and reports received bytes', async () => {
  const events = [];
  const server = await startUdpSpeedServer({
    host: '127.0.0.1',
    port: 0,
    onEvent: (event) => events.push(event)
  });

  try {
    await runUdpSpeedClient({
      host: '127.0.0.1',
      port: server.port,
      bytes: 64 * 1024,
      packetSize: 1200,
      onEvent: (event) => events.push(event)
    });
  } finally {
    await server.close();
  }

  const clientFinal = events.find((event) => event.type === 'udp_speed' && event.side === 'client' && event.final);
  const serverFinal = events.find((event) => event.type === 'udp_speed' && event.side === 'server' && event.final);
  const peer = events.find((event) => event.type === 'udp_peer' && event.side === 'server');

  assert.equal(clientFinal?.bytes, 64 * 1024);
  assert.equal(serverFinal?.bytes, 64 * 1024);
  assert.equal(clientFinal?.packetSize, 1200);
  assert.equal(clientFinal?.remoteBytes, 64 * 1024);
  assert.equal(clientFinal?.lossPercent, 0);
  assert.equal(peer?.remoteAddress, '127.0.0.1');
});

test('udp speed client auto-detects the server port from a candidate range', async () => {
  const events = [];
  const server = await startUdpSpeedServer({
    host: '127.0.0.1',
    port: 0,
    onEvent: (event) => events.push(event)
  });

  try {
    await runUdpSpeedClient({
      host: '127.0.0.1',
      autoPort: true,
      portRange: [server.port + 1, server.port, server.port + 2],
      bytes: 8 * 1024,
      packetSize: 1024,
      onEvent: (event) => events.push(event)
    });
  } finally {
    await server.close();
  }

  const detected = events.find((event) => event.type === 'udp_port_detected');
  const clientFinal = events.find((event) => event.type === 'udp_speed' && event.side === 'client' && event.final);

  assert.equal(detected?.port, server.port);
  assert.equal(clientFinal?.port, server.port);
});
