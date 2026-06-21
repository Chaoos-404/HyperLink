import os from 'node:os';
import multicastdns from 'multicast-dns';
import { DEFAULT_PORT } from './protocol.js';
import { listInterfaces } from './interfaces.js';

const SERVICE = '_hyperlink._tcp.local';

export function advertisePeer({ port = DEFAULT_PORT, name = os.hostname(), tokenRequired = false } = {}) {
  const mdns = multicastdns({ multicast: true, loopback: true });
  const instance = `${sanitizeName(name)}.${SERVICE}`;

  mdns.on('query', (query) => {
    const wantsService = query.questions.some((question) => question.name === SERVICE || question.name === instance);
    if (!wantsService) return;
    mdns.respond(buildRecords({ instance, port, name, tokenRequired }));
  });

  mdns.respond(buildRecords({ instance, port, name, tokenRequired }));

  return {
    close: () => mdns.destroy(),
    service: SERVICE,
    instance
  };
}

export async function discoverPeers({ timeoutMs = 3000 } = {}) {
  const mdns = multicastdns({ multicast: true, loopback: true });
  const peers = new Map();

  mdns.on('response', (response) => {
    const records = [...(response.answers ?? []), ...(response.additionals ?? [])];
    const ptrs = records.filter((record) => record.type === 'PTR' && record.name === SERVICE);

    for (const ptr of ptrs) {
      const instance = ptr.data;
      const srv = records.find((record) => record.type === 'SRV' && record.name === instance);
      const txt = records.find((record) => record.type === 'TXT' && record.name === instance);
      const addresses = records
        .filter((record) => (record.type === 'A' || record.type === 'AAAA') && (!srv || record.name === srv.data.target))
        .map((record) => scopedAddress(record));

      peers.set(instance, {
        instance,
        name: instance.replace(`.${SERVICE}`, ''),
        host: addresses[0] ?? srv?.data?.target,
        addresses,
        port: srv?.data?.port ?? DEFAULT_PORT,
        tokenRequired: parseTxt(txt).tokenRequired === 'true'
      });
    }
  });

  mdns.query([{ name: SERVICE, type: 'PTR' }]);
  await new Promise((resolve) => setTimeout(resolve, timeoutMs));
  mdns.destroy();

  return [...peers.values()].filter((peer) => peer.host);
}

function buildRecords({ instance, port, name, tokenRequired }) {
  const host = `${sanitizeName(os.hostname())}.local`;
  const candidates = listInterfaces().filter((entry) => entry.pointToPointCandidate);
  const preferredFamily = candidates.some((entry) => entry.family === 'IPv4') ? 'IPv4' : 'IPv6';
  const addresses = candidates.filter((entry) => entry.family === preferredFamily);
  const additionals = addresses.map((entry) => ({
    name: host,
    type: entry.family === 'IPv4' ? 'A' : 'AAAA',
    ttl: 5,
    data: entry.address
  }));

  return {
    answers: [
      { name: SERVICE, type: 'PTR', ttl: 5, data: instance },
      {
        name: instance,
        type: 'SRV',
        ttl: 5,
        data: { priority: 0, weight: 100, port, target: host }
      },
      {
        name: instance,
        type: 'TXT',
        ttl: 5,
        data: [`name=${name}`, `tokenRequired=${tokenRequired}`]
      }
    ],
    additionals
  };
}

function parseTxt(record) {
  const result = {};
  for (const item of record?.data ?? []) {
    const text = Buffer.isBuffer(item) ? item.toString('utf8') : String(item);
    const index = text.indexOf('=');
    if (index !== -1) result[text.slice(0, index)] = text.slice(index + 1);
  }
  return result;
}

function scopedAddress(record) {
  if (record.type !== 'AAAA') return record.data;
  const match = listInterfaces().find((entry) => entry.family === 'IPv6' && entry.address === record.data);
  return match ? `${record.data}%${match.name}` : record.data;
}

function sanitizeName(name) {
  return String(name).replace(/[^a-zA-Z0-9-]/g, '-').slice(0, 48) || 'hyperlink';
}
