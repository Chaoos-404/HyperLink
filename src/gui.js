import http from 'node:http';
import { createReadStream } from 'node:fs';
import fsp from 'node:fs/promises';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { DEFAULT_PORT } from './protocol.js';
import { discoverPeers } from './discovery.js';
import { listInterfaces } from './interfaces.js';
import { sendVirtualFile } from './sender.js';

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const PUBLIC_DIR = path.resolve(__dirname, '..', 'public');

export async function startGui({ port = 44880 } = {}) {
  const server = http.createServer(async (req, res) => {
    try {
      const url = new URL(req.url, `http://${req.headers.host}`);

      if (req.method === 'GET' && url.pathname === '/api/interfaces') {
        return json(res, listInterfaces());
      }

      if (req.method === 'GET' && url.pathname === '/api/discover') {
        return json(res, await discoverPeers({ timeoutMs: Number(url.searchParams.get('timeout') ?? 2500) }));
      }

      if (req.method === 'PUT' && url.pathname === '/api/send-upload') {
        const host = required(url, 'host');
        const remotePort = Number(url.searchParams.get('port') ?? DEFAULT_PORT);
        const relativePath = sanitizeUploadPath(required(url, 'path'));
        const size = Number(url.searchParams.get('size') ?? req.headers['content-length'] ?? 0);
        await sendVirtualFile({ stream: req, relativePath, size, host, port: remotePort });
        return json(res, { ok: true });
      }

      return await serveStatic(url.pathname, res);
    } catch (error) {
      res.writeHead(500, { 'content-type': 'application/json' });
      res.end(JSON.stringify({ error: error.message }));
    }
  });

  await new Promise((resolve, reject) => {
    server.once('error', reject);
    server.listen(port, '127.0.0.1', () => {
      server.off('error', reject);
      resolve();
    });
  });

  return {
    server,
    url: `http://127.0.0.1:${server.address().port}/`,
    close: () => new Promise((resolve) => server.close(resolve))
  };
}

async function serveStatic(requestPath, res) {
  const pathname = requestPath === '/' ? '/index.html' : requestPath;
  const target = path.normalize(path.join(PUBLIC_DIR, pathname));
  if (!target.startsWith(PUBLIC_DIR)) {
    res.writeHead(403);
    res.end('Forbidden');
    return;
  }

  await fsp.access(target);
  res.writeHead(200, { 'content-type': contentType(target) });
  createReadStream(target).pipe(res);
}

function json(res, value) {
  res.writeHead(200, { 'content-type': 'application/json' });
  res.end(JSON.stringify(value));
}

function required(url, key) {
  const value = url.searchParams.get(key);
  if (!value) throw new Error(`Missing required query parameter: ${key}`);
  return value;
}

function sanitizeUploadPath(value) {
  return value.replaceAll('\\', '/').replace(/^\/+/, '') || 'upload.bin';
}

function contentType(target) {
  if (target.endsWith('.html')) return 'text/html; charset=utf-8';
  if (target.endsWith('.css')) return 'text/css; charset=utf-8';
  if (target.endsWith('.js')) return 'text/javascript; charset=utf-8';
  return 'application/octet-stream';
}
