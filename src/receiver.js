import fs from 'node:fs';
import fsp from 'node:fs/promises';
import net from 'node:net';
import path from 'node:path';
import { execFile } from 'node:child_process';
import { promisify } from 'node:util';
import { FrameReader, PROTOCOL_VERSION, writeFrame } from './protocol.js';
import { FALLBACK_HASH_ALGORITHM, chooseHashAlgorithm, createFastHash, hashBuffer, initHashing } from './hash.js';
import { advertisePeer } from './discovery.js';

const execFileAsync = promisify(execFile);

export async function startReceiver({
  dest = 'received',
  port = 44777,
  host,
  token,
  advertise = true,
  overwrite = false,
  onEvent = () => {}
} = {}) {
  await initHashing();
  await fsp.mkdir(dest, { recursive: true });

  const server = net.createServer((socket) => {
    socket.setNoDelay(true);
    socket.setKeepAlive(true, 5000);
    receiveConnection(socket, { dest, token, overwrite, onEvent }).catch((error) => {
      onEvent({ type: 'error', message: error.message });
      socket.destroy(error);
    });
  });

  await new Promise((resolve, reject) => {
    server.once('error', reject);
    server.listen(port, host, () => {
      server.off('error', reject);
      resolve();
    });
  });

  const discovery = advertise ? advertisePeer({ port, tokenRequired: Boolean(token) }) : null;
  return {
    server,
    port: server.address().port,
    close: async () => {
      discovery?.close();
      await new Promise((resolve) => server.close(resolve));
    }
  };
}

export async function receiveConnection(socket, { dest, token, overwrite = false, onEvent = () => {} }) {
  await initHashing();
  const reader = new FrameReader(socket);
  let current = null;
  let hashAlgorithm = FALLBACK_HASH_ALGORITHM;

  for (;;) {
    const frame = await reader.readFrame();
    if (!frame) break;
    const { header, payload } = frame;

    if (header.type === 'hello') {
      if (header.version !== PROTOCOL_VERSION) {
        await writeFrame(socket, { type: 'error', message: `Unsupported protocol version ${header.version}` });
        socket.end();
        return;
      }
      if (token && header.token !== token) {
        await writeFrame(socket, { type: 'error', message: 'Invalid transfer token' });
        socket.end();
        return;
      }
      hashAlgorithm = chooseHashAlgorithm(header.supportedHashAlgorithms ?? []);
      onEvent({ type: 'peer', agent: header.agent, remote: socket.remoteAddress });
      await writeFrame(socket, {
        type: 'hello_ack',
        version: PROTOCOL_VERSION,
        hashAlgorithm,
        supportsPipelining: true
      });
      continue;
    }

    if (header.type === 'dir') {
      const target = safeTarget(dest, header.path);
      await fsp.mkdir(target, { recursive: true });
      await writeFrame(socket, { type: 'dir_ack', path: header.path });
      onEvent({ type: 'dir', path: header.path });
      continue;
    }

    if (header.type === 'file') {
      if (current) throw new Error('Received a new file before the previous file ended');
      const finalPath = await chooseTarget(safeTarget(dest, header.path), overwrite);
      const available = await availableBytes(path.dirname(finalPath));
      if (Number.isFinite(header.size) && available !== null && header.size > available) {
        await writeFrame(socket, { type: 'error', message: `Insufficient disk space for ${header.path}` });
        socket.end();
        return;
      }

      await fsp.mkdir(path.dirname(finalPath), { recursive: true });
      const tempPath = `${finalPath}.boltbridge-part`;
      const handle = await fsp.open(tempPath, 'w');
      current = {
        path: header.path,
        finalPath,
        tempPath,
        handle,
        bytes: 0,
        nextIndex: 0,
        size: header.size,
        mode: header.mode,
        mtimeMs: header.mtimeMs,
        hash: createFastHash(hashAlgorithm)
      };
      await writeFrame(socket, { type: 'file_ready', path: header.path });
      onEvent({ type: 'file_start', path: header.path, size: header.size });
      continue;
    }

    if (header.type === 'block') {
      if (!current) throw new Error('Received a block outside of a file');
      if (header.index !== current.nextIndex) {
        await writeFrame(socket, { type: 'block_nack', path: current.path, index: header.index, message: 'Unexpected block index' });
        continue;
      }

      const actualHash = hashBuffer(payload, hashAlgorithm);
      if (actualHash !== header.hash) {
        await writeFrame(socket, { type: 'block_nack', path: current.path, index: header.index, message: 'Block hash mismatch' });
        continue;
      }

      await current.handle.write(payload);
      current.hash.update(payload);
      current.bytes += payload.length;
      current.nextIndex += 1;
      await writeFrame(socket, { type: 'block_ack', path: current.path, index: header.index });
      onEvent({ type: 'bytes', path: current.path, bytes: payload.length });
      continue;
    }

    if (header.type === 'file_end') {
      if (!current) throw new Error('Received file_end outside of a file');
      const digest = current.hash.digest('hex');
      await current.handle.close();
      if (current.bytes !== current.size || digest !== header.hash) {
        await fsp.rm(current.tempPath, { force: true });
        await writeFrame(socket, { type: 'error', message: `Integrity check failed for ${current.path}` });
        socket.end();
        return;
      }
      await fsp.rename(current.tempPath, current.finalPath);
      if (current.mode) await fsp.chmod(current.finalPath, current.mode).catch(() => {});
      if (current.mtimeMs) {
        const mtime = new Date(current.mtimeMs);
        await fsp.utimes(current.finalPath, mtime, mtime).catch(() => {});
      }
      await writeFrame(socket, { type: 'file_end_ack', path: current.path, hash: digest });
      onEvent({ type: 'file_done', path: current.path, bytes: current.bytes });
      current = null;
      continue;
    }

    if (header.type === 'done') {
      await writeFrame(socket, { type: 'done_ack' });
      onEvent({ type: 'done' });
      socket.end();
      return;
    }
  }
}

function safeTarget(root, relativePath) {
  const normalized = path.normalize(`/${relativePath}`).slice(1);
  if (!normalized || normalized.startsWith('..') || path.isAbsolute(normalized)) {
    throw new Error(`Unsafe transfer path: ${relativePath}`);
  }
  return path.join(root, normalized);
}

async function chooseTarget(target, overwrite) {
  if (overwrite) return target;
  const parsed = path.parse(target);
  let candidate = target;
  let index = 1;
  while (fs.existsSync(candidate) || fs.existsSync(`${candidate}.boltbridge-part`)) {
    candidate = path.join(parsed.dir, `${parsed.name} (${index})${parsed.ext}`);
    index += 1;
  }
  return candidate;
}

async function availableBytes(dir) {
  try {
    const { stdout } = await execFileAsync('df', ['-Pk', dir]);
    const lines = stdout.trim().split(/\r?\n/);
    const columns = lines.at(-1).trim().split(/\s+/);
    return Number(columns[3]) * 1024;
  } catch {
    return null;
  }
}
