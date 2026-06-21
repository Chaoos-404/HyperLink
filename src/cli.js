#!/usr/bin/env node
import { DEFAULT_BLOCK_SIZE, DEFAULT_PIPELINE_WINDOW, DEFAULT_PORT } from './protocol.js';
import { discoverPeers } from './discovery.js';
import { listInterfaces } from './interfaces.js';
import { Progress, formatBytes } from './progress.js';
import { startReceiver } from './receiver.js';
import { sendPaths } from './sender.js';
import { startGui } from './gui.js';
import { printDoctorReport, runDoctor } from './doctor.js';

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
      retries: numberOption(options.retries, 2),
      diagnostics: Boolean(options.diagnose || options.verbose),
      onEvent: (event) => {
        if (event.type === 'file_start') progress.setFile(event.path);
        if (event.type === 'bytes') progress.add(event.bytes);
        if (event.type === 'negotiated' && (options.diagnose || options.verbose)) {
          console.error(
            `negotiated hash=${event.hashAlgorithm} block=${formatBytes(event.blockSize)} window=${event.pipelineWindow}` +
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
  hyperlink send <file-or-dir...> [--host address] [--port ${DEFAULT_PORT}] [--bind link-local-address] [--window ${DEFAULT_PIPELINE_WINDOW}] [--diagnose]
  hyperlink discover [--timeout 3000]
  hyperlink interfaces
  hyperlink doctor [--host address]
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
