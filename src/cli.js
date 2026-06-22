#!/usr/bin/env node
import { DEFAULT_BLOCK_SIZE, DEFAULT_PIPELINE_WINDOW, DEFAULT_PORT, DEFAULT_SOCKET_HIGH_WATER_MARK } from './protocol.js';
import { discoverPeers } from './discovery.js';
import { listInterfaces } from './interfaces.js';
import { Progress, formatBytes } from './progress.js';
import { startReceiver } from './receiver.js';
import { sendPaths } from './sender.js';
import { startGui } from './gui.js';
import { printDoctorReport, runDoctor } from './doctor.js';
import { defaultSpeedBytes, defaultSpeedChunkSize, defaultSpeedPort, runSpeedClient, startSpeedServer } from './speedtest.js';
import { defaultUdpPacketSize, defaultUdpSpeedBytes, defaultUdpSpeedPort, runUdpSpeedClient, startUdpSpeedServer } from './udpspeed.js';

main().catch((error) => {
  console.error(`boltbridge: ${error.message}`);
  process.exitCode = 1;
});

async function main() {
  const { command, options, positionals } = parseArgs(process.argv.slice(2));

  if (!command || options.help || command === 'help') {
    printHelp();
    return;
  }

  if (command === 'interfaces') {
    const rows = listInterfaces();
    for (const row of rows) {
      const marker = row.pointToPointCandidate ? '*' : row.linkLocal ? '~' : ' ';
      console.log(`${marker} ${row.name.padEnd(14)} ${row.family.padEnd(4)} ${row.address.padEnd(39)} score=${row.score}`);
    }
    return;
  }

  if (command === 'discover') {
    const peers = await discoverPeers({ timeoutMs: numberOption(options.timeout, 3000) });
    if (!peers.length) {
      console.log('No peers discovered.');
      return;
    }
    for (const peer of peers) {
      console.log(`${peer.name} ${peer.host}:${peer.port}${peer.tokenRequired ? ' token-required' : ''}`);
    }
    return;
  }

  if (command === 'doctor') {
    const report = await runDoctor({ host: options.host });
    printDoctorReport(report);
    return;
  }

  if (command === 'serve') {
    const progress = new Progress({ label: 'receive' });
    const receiver = await startReceiver({
      dest: options.dest ?? 'received',
      port: numberOption(options.port, DEFAULT_PORT),
      host: options.host,
      token: options.token,
      advertise: options.discovery !== false,
      overwrite: Boolean(options.overwrite),
      diagnostics: Boolean(options.diagnose || options.verbose),
      socketHighWaterMark: parseSize(options.socketBuffer ?? options['socket-buffer'], DEFAULT_SOCKET_HIGH_WATER_MARK),
      onEvent: (event) => {
        if (event.type === 'file_start') {
          progress.reset();
          progress.setFile(event.path);
        }
        if (event.type === 'bytes') progress.add(event.bytes);
        if (event.type === 'done') progress.done();
        if (event.type === 'error') console.error(event.message);
        if (event.type === 'peer' && (options.diagnose || options.verbose)) {
          console.error(`peer agent=${event.agent ?? 'unknown'} remote=${event.remote ?? 'unknown'}`);
        }
        if (event.type === 'diagnostic') printDiagnostic(event);
      }
    });
    console.log(`HyperLink receiver listening on port ${receiver.port}; writing to ${options.dest ?? 'received'}`);
    await waitForever();
    return;
  }

  if (command === 'speed-server') {
    const server = await startSpeedServer({
      host: options.host,
      port: numberOption(options.port, defaultSpeedPort()),
      socketHighWaterMark: parseSize(options.socketBuffer ?? options['socket-buffer'], DEFAULT_SOCKET_HIGH_WATER_MARK),
      onEvent: printSpeedEvent
    });
    console.log(`HyperLink speed server listening on port ${server.port}`);
    await waitForever();
    return;
  }

  if (command === 'speed-client') {
    await runSpeedClient({
      host: options.host,
      port: numberOption(options.port, defaultSpeedPort()),
      autoPort: Boolean(options.autoPort ?? options['auto-port']),
      portRange: parsePortRange(options.portRange ?? options['port-range']),
      bytes: parseSize(options.bytes ?? options.size, defaultSpeedBytes()),
      chunkSize: parseSize(options.chunkSize ?? options['chunk-size'], defaultSpeedChunkSize()),
      socketHighWaterMark: parseSize(options.socketBuffer ?? options['socket-buffer'], DEFAULT_SOCKET_HIGH_WATER_MARK),
      onEvent: printSpeedEvent
    });
    return;
  }

  if (command === 'udp-speed-server') {
    const server = await startUdpSpeedServer({
      host: options.host,
      port: numberOption(options.port, defaultUdpSpeedPort()),
      onEvent: printSpeedEvent
    });
    console.log(`HyperLink UDP speed server listening on port ${server.port}`);
    await waitForever();
    return;
  }

  if (command === 'udp-speed-client') {
    await runUdpSpeedClient({
      host: options.host,
      port: numberOption(options.port, defaultUdpSpeedPort()),
      autoPort: Boolean(options.autoPort ?? options['auto-port']),
      portRange: parsePortRange(options.portRange ?? options['port-range']),
      bytes: parseSize(options.bytes ?? options.size, defaultUdpSpeedBytes()),
      packetSize: parseSize(options.packetSize ?? options['packet-size'], defaultUdpPacketSize()),
      onEvent: printSpeedEvent
    });
    return;
  }

  if (command === 'send') {
    const progress = new Progress({ label: 'send' });
    await sendPaths({
      paths: positionals,
      host: options.host,
      port: numberOption(options.port, DEFAULT_PORT),
      bind: options.bind,
      token: options.token,
      blockSize: parseSize(options.blockSize ?? options['block-size'], DEFAULT_BLOCK_SIZE),
      pipelineWindow: numberOption(options.window ?? options.pipelineWindow, DEFAULT_PIPELINE_WINDOW),
      socketHighWaterMark: parseSize(options.socketBuffer ?? options['socket-buffer'], null),
      retries: numberOption(options.retries, 2),
      diagnostics: Boolean(options.diagnose || options.verbose),
      onEvent: (event) => {
        if (event.type === 'file_start') progress.setFile(event.path);
        if (event.type === 'bytes') progress.add(event.bytes);
        if (event.type === 'negotiated' && (options.diagnose || options.verbose)) {
          console.error(
            `negotiated hash=${event.hashAlgorithm} block=${formatBytes(event.blockSize)} window=${event.pipelineWindow}` +
            ` socketBuffer=${formatBytes(event.socketWritableHighWaterMark)}` +
            `${event.legacyPeer ? ' legacy-peer=true' : ''}`
          );
        }
        if (event.type === 'diagnostic') printDiagnostic(event);
      }
    });
    progress.done();
    console.log(`Sent ${formatBytes(progress.bytes)}.`);
    return;
  }

  if (command === 'gui') {
    const gui = await startGui({ port: numberOption(options.port, 44880) });
    console.log(`HyperLink GUI: ${gui.url}`);
    await waitForever();
    return;
  }

  throw new Error(`Unknown command: ${command}`);
}

function parseArgs(argv) {
  const [command, ...rest] = argv;
  const options = {};
  const positionals = [];

  for (let index = 0; index < rest.length; index += 1) {
    const arg = rest[index];
    if (!arg.startsWith('--')) {
      positionals.push(arg);
      continue;
    }

    const [rawKey, inlineValue] = arg.slice(2).split('=', 2);
    const key = camel(rawKey);
    if (rawKey.startsWith('no-')) {
      options[camel(rawKey.slice(3))] = false;
    } else if (inlineValue !== undefined) {
      options[key] = inlineValue;
    } else if (rest[index + 1] && !rest[index + 1].startsWith('--')) {
      options[key] = rest[index + 1];
      index += 1;
    } else {
      options[key] = true;
    }
  }

  return { command, options, positionals };
}

function numberOption(value, fallback) {
  if (value === undefined || value === true || value === false) return fallback;
  const number = Number(value);
  if (!Number.isFinite(number)) throw new Error(`Invalid number: ${value}`);
  return number;
}

function parseSize(value, fallback) {
  if (!value) return fallback;
  const match = String(value).match(/^(\d+(?:\.\d+)?)(b|kb|mb|gb)?$/i);
  if (!match) throw new Error(`Invalid size: ${value}`);
  const number = Number(match[1]);
  const unit = (match[2] ?? 'b').toLowerCase();
  const multiplier = { b: 1, kb: 1024, mb: 1024 ** 2, gb: 1024 ** 3 }[unit];
  return Math.floor(number * multiplier);
}

function parsePortRange(value) {
  if (value === undefined || value === true || value === false) return undefined;
  const text = String(value).trim();
  if (!text) return undefined;
  const ports = [];
  for (const part of text.split(',')) {
    const trimmed = part.trim();
    const range = trimmed.match(/^(\d+)-(\d+)$/);
    if (range) {
      const start = Number(range[1]);
      const end = Number(range[2]);
      if (!validPort(start) || !validPort(end) || end < start) throw new Error(`Invalid port range: ${trimmed}`);
      for (let port = start; port <= end; port += 1) ports.push(port);
      continue;
    }

    const port = Number(trimmed);
    if (!validPort(port)) throw new Error(`Invalid port: ${trimmed}`);
    ports.push(port);
  }
  return ports;
}

function validPort(port) {
  return Number.isInteger(port) && port > 0 && port <= 65535;
}

function camel(value) {
  return value.replace(/-([a-z])/g, (_, char) => char.toUpperCase());
}

function waitForever() {
  return new Promise(() => {});
}

function printHelp() {
  console.log(`HyperLink

Usage:
  hyperlink serve [--dest received] [--port ${DEFAULT_PORT}] [--token secret] [--overwrite] [--diagnose]
  hyperlink send <file-or-dir...> [--host address] [--port ${DEFAULT_PORT}] [--bind link-local-address] [--window ${DEFAULT_PIPELINE_WINDOW}] [--socket-buffer 64mb] [--diagnose]
  hyperlink discover [--timeout 3000]
  hyperlink interfaces
  hyperlink doctor [--host address]
  hyperlink speed-server [--port ${defaultSpeedPort()}] [--socket-buffer 64mb]
  hyperlink speed-client --host address [--port ${defaultSpeedPort()}] [--auto-port] [--port-range ${defaultSpeedPort()}-${defaultSpeedPort() + 20}] [--bytes 512mb] [--chunk-size 4mb] [--socket-buffer 64mb]
  hyperlink udp-speed-server [--port ${defaultUdpSpeedPort()}]
  hyperlink udp-speed-client --host address [--port ${defaultUdpSpeedPort()}] [--auto-port] [--port-range ${defaultUdpSpeedPort()}-${defaultUdpSpeedPort() + 20}] [--bytes 512mb] [--packet-size 1200]
  hyperlink gui [--port 44880]

Notes:
  - Prefer link-local addresses such as 169.254.x.x or fe80::/10 for wired point-to-point links.
  - Transfers are streamed; files are not indexed into memory before sending.
  - Blocks are pipelined, hashed, and acknowledged; use --window 1 for stop-and-wait retry mode.`);
}

function printDiagnostic(event) {
  const parts = [
    event.side ?? 'sender',
    event.final ? 'final' : 'live',
    `path=${event.path}`,
    `rate=${formatBytes(event.throughputBytesPerSecond)}/s`,
    `hash=${(event.hashMs ?? 0).toFixed(1)}ms`
  ];

  if (event.writeMs !== undefined) parts.push(`socketWrite=${event.writeMs.toFixed(1)}ms`);
  if (event.ackWaitMs !== undefined) parts.push(`ackWait=${event.ackWaitMs.toFixed(1)}ms`);
  if (event.diskWriteMs !== undefined) parts.push(`diskWrite=${event.diskWriteMs.toFixed(1)}ms`);
  if (event.ackWriteMs !== undefined) parts.push(`ackWrite=${event.ackWriteMs.toFixed(1)}ms`);
  if (event.blocksAcked !== undefined) parts.push(`acked=${event.blocksAcked}`);
  if (event.blocks !== undefined) parts.push(`blocks=${event.blocks}`);

  console.error(parts.join(' '));
}

function printSpeedEvent(event) {
  if (event.type === 'speed_port_detected' || event.type === 'udp_port_detected') {
    console.error(`${event.type === 'udp_port_detected' ? 'udp ' : ''}speed server detected host=${event.host} port=${event.port}`);
    return;
  }

  if (event.type === 'speed_peer' || event.type === 'udp_peer') {
    console.error(
      `${event.side} peer local=${event.localAddress ?? 'unknown'}:${event.localPort ?? 'unknown'} ` +
      `remote=${event.remoteAddress ?? 'unknown'}:${event.remotePort ?? 'unknown'} ` +
      `${event.writableHighWaterMark ? `socketBuffer=${formatBytes(event.writableHighWaterMark)}` : ''}`
    );
    return;
  }

  const parts = [
    event.side,
    event.final ? 'final' : 'live',
    `bytes=${formatBytes(event.bytes)}`,
    `rate=${formatBytes(event.throughputBytesPerSecond)}/s`
  ];
  if (event.writeMs) parts.push(`socketWrite=${event.writeMs.toFixed(1)}ms`);
  if (event.sendMs) parts.push(`udpSend=${event.sendMs.toFixed(1)}ms`);
  if (event.packets !== undefined) parts.push(`packets=${event.packets}`);
  if (event.packetSize !== undefined) parts.push(`packetSize=${formatBytes(event.packetSize)}`);
  if (event.remoteBytes !== undefined) {
    parts.push(`received=${formatBytes(event.remoteBytes)}`);
    parts.push(`receiveRate=${formatBytes(event.remoteThroughputBytesPerSecond)}/s`);
    parts.push(`loss=${event.lossPercent.toFixed(2)}%`);
  }
  console.error(parts.join(' '));
}
