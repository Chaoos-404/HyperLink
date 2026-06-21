import os from 'node:os';

export function isIPv4LinkLocal(address) {
  return address.startsWith('169.254.');
}

export function isIPv6LinkLocal(address) {
  return address.toLowerCase().startsWith('fe80:');
}

export function isLinkLocal(address, family) {
  return family === 'IPv4' ? isIPv4LinkLocal(address) : isIPv6LinkLocal(address);
}

export function interfaceScore(name) {
  const value = name.toLowerCase();
  if (value.includes('thunderbolt') || value.includes('bridge')) return 100;
  if (value.includes('usb')) return 90;
  if (value.startsWith('en') || value.startsWith('eth')) return 60;
  if (value.startsWith('wl') || value.includes('wi-fi') || value.includes('wifi')) return 20;
  return 40;
}

export function isLikelyVirtualInterface(name) {
  const value = name.toLowerCase();
  return (
    value.startsWith('utun') ||
    value.startsWith('awdl') ||
    value.startsWith('llw') ||
    value.startsWith('lo') ||
    value.startsWith('docker') ||
    value.startsWith('veth')
  );
}

export function isPointToPointCandidate(entry) {
  return entry.linkLocal && !isLikelyVirtualInterface(entry.name);
}

export function listInterfaces() {
  const rows = [];
  const interfaces = os.networkInterfaces();

  for (const [name, entries] of Object.entries(interfaces)) {
    for (const entry of entries ?? []) {
      if (entry.internal) continue;
      const family = normalizeFamily(entry.family);
      if (family !== 'IPv4' && family !== 'IPv6') continue;

      rows.push({
        name,
        address: entry.address,
        family,
        mac: entry.mac,
        netmask: entry.netmask,
        cidr: entry.cidr,
        linkLocal: isLinkLocal(entry.address, family),
        pointToPointCandidate: false,
        score: interfaceScore(name)
      });
    }
  }

  for (const row of rows) {
    row.pointToPointCandidate = isPointToPointCandidate(row);
  }

  return rows.sort((a, b) => {
    if (a.pointToPointCandidate !== b.pointToPointCandidate) return a.pointToPointCandidate ? -1 : 1;
    if (a.linkLocal !== b.linkLocal) return a.linkLocal ? -1 : 1;
    return b.score - a.score;
  });
}

export function bestLinkLocalAddress(preferFamily = 'IPv4') {
  const candidates = listInterfaces().filter((entry) => entry.pointToPointCandidate && entry.family === preferFamily);
  return candidates[0]?.address ?? null;
}

function normalizeFamily(family) {
  if (family === 4) return 'IPv4';
  if (family === 6) return 'IPv6';
  return family;
}
