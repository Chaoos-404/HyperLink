import assert from 'node:assert/strict';
import fsp from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import test from 'node:test';
import { startReceiver } from '../src/receiver.js';
import { sendPaths } from '../src/sender.js';

test('streams a nested directory to a receiver', async () => {
  const temp = await fsp.mkdtemp(path.join(os.tmpdir(), 'boltbridge-'));
  const source = path.join(temp, 'source');
  const dest = path.join(temp, 'dest');
  await fsp.mkdir(path.join(source, 'nested'), { recursive: true });
  await fsp.writeFile(path.join(source, 'hello.txt'), 'hello boltbridge');
  await fsp.writeFile(path.join(source, 'nested', 'data.bin'), Buffer.alloc(128 * 1024, 7));

  const receiver = await startReceiver({ dest, port: 0, advertise: false });
  try {
    await sendPaths({
      paths: [source],
      host: '127.0.0.1',
      port: receiver.port,
      blockSize: 16 * 1024
    });

    assert.equal(await fsp.readFile(path.join(dest, 'source', 'hello.txt'), 'utf8'), 'hello boltbridge');
    assert.equal((await fsp.readFile(path.join(dest, 'source', 'nested', 'data.bin'))).length, 128 * 1024);
  } finally {
    await receiver.close();
    await fsp.rm(temp, { recursive: true, force: true });
  }
});
