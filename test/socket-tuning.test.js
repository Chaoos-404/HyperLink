import assert from 'node:assert/strict';
import fsp from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import test from 'node:test';
import { startReceiver } from '../src/receiver.js';
import { sendVirtualFile } from '../src/sender.js';
import { runSpeedClient, startSpeedServer } from '../src/speedtest.js';

test('speed test applies socket buffer and chunk size tuning', async () => {
  const events = [];
  const server = await startSpeedServer({
    port: 0,
    host: '127.0.0.1',
    socketHighWaterMark: 2 * 1024 * 1024,
    onEvent: (event) => events.push(event)
  });

  try {
    await runSpeedClient({
      host: '127.0.0.1',
      port: server.port,
      bytes: 256 * 1024,
      chunkSize: 64 * 1024,
      socketHighWaterMark: 3 * 1024 * 1024,
      onEvent: (event) => events.push(event)
    });
  } finally {
    await server.close();
  }

  const serverPeer = events.find((event) => event.type === 'speed_peer' && event.side === 'server');
  const clientPeer = events.find((event) => event.type === 'speed_peer' && event.side === 'client');
  const final = events.find((event) => event.type === 'speed' && event.side === 'client' && event.final);

  assert.equal(serverPeer?.readableHighWaterMark, 2 * 1024 * 1024);
  assert.equal(clientPeer?.writableHighWaterMark, 3 * 1024 * 1024);
  assert.equal(final?.chunkSize, 64 * 1024);
});

test('file transfer applies configured socket buffer tuning', async () => {
  const temp = await fsp.mkdtemp(path.join(os.tmpdir(), 'hyperlink-socket-tuning-'));
  const dest = path.join(temp, 'received');
  const events = [];
  const receiver = await startReceiver({
    port: 0,
    host: '127.0.0.1',
    dest,
    advertise: false,
    socketHighWaterMark: 2 * 1024 * 1024,
    onEvent: (event) => events.push(event)
  });

  try {
    await sendVirtualFile({
      host: '127.0.0.1',
      port: receiver.port,
      relativePath: 'virtual.bin',
      size: 128 * 1024,
      stream: ReadableStreamToAsyncIterable(new Blob([Buffer.alloc(128 * 1024, 1)]).stream()),
      socketHighWaterMark: 3 * 1024 * 1024,
      onEvent: (event) => events.push(event)
    });
  } finally {
    await receiver.close();
    await fsp.rm(temp, { recursive: true, force: true });
  }

  const peer = events.find((event) => event.type === 'peer');
  const negotiated = events.find((event) => event.type === 'negotiated');

  assert.equal(peer?.readableHighWaterMark, 2 * 1024 * 1024);
  assert.equal(negotiated?.socketWritableHighWaterMark, 3 * 1024 * 1024);
});

async function* ReadableStreamToAsyncIterable(stream) {
  const reader = stream.getReader();
  try {
    for (;;) {
      const { done, value } = await reader.read();
      if (done) return;
      yield value;
    }
  } finally {
    reader.releaseLock();
  }
}
