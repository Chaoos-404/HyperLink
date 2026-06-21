import { execFile } from 'node:child_process';
import os from 'node:os';
import { promisify } from 'node:util';
import { listInterfaces } from './interfaces.js';

const execFileAsync = promisify(execFile);

export async function runDoctor({ host } = {}) {
  const platform = os.platform();
  const report = {
    platform,
    interfaces: listInterfaces(),
    route: null,
    warnings: []
  };

  if (host && platform === 'darwin') {
    report.route = await commandOrNull('route', ['-n', 'get', host]);
    const routeInterface = parseDarwinRouteInterface(report.route);
    if (routeInterface) {
      report.routeInterface = routeInterface;
      report.interfaceDetail = await commandOrNull('ifconfig', [routeInterface]);
    }
  }

  if (host && platform === 'win32') {
    report.route = await commandOrNull('powershell.exe', [
      '-NoProfile',
      '-Command',
      `Find-NetRoute -RemoteIPAddress '${host}' | Format-List *`
    ]);
  }

  const routedInterface = report.routeInterface
    ? report.interfaces.find((entry) => entry.name === report.routeInterface)
    : null;

  if (host && routedInterface && !routedInterface.pointToPointCandidate) {
    report.warnings.push(`Route to ${host} uses ${routedInterface.name}, which is not marked as a point-to-point candidate.`);
  }

  if (report.interfaceDetail?.includes('mtu 1500')) {
    report.warnings.push('The routed interface has MTU 1500. This is compatible, but jumbo frames can improve high-speed bulk transfer when both OSes support them.');
  }

  return report;
}

export function printDoctorReport(report) {
  console.log(`Platform: ${report.platform}`);

  if (report.routeInterface) {
    console.log(`Route interface: ${report.routeInterface}`);
  }

  console.log('\nPoint-to-point candidates:');
  for (const entry of report.interfaces.filter((item) => item.pointToPointCandidate)) {
    console.log(`  ${entry.name} ${entry.family} ${entry.address} score=${entry.score}`);
  }

  if (report.route) {
    console.log('\nRoute:');
    console.log(indent(report.route.trim()));
  }

  if (report.interfaceDetail) {
    console.log('\nInterface detail:');
    console.log(indent(report.interfaceDetail.trim()));
  }

  if (report.warnings.length) {
    console.log('\nWarnings:');
    for (const warning of report.warnings) console.log(`  - ${warning}`);
  }
}

async function commandOrNull(command, args) {
  try {
    const { stdout, stderr } = await execFileAsync(command, args);
    return `${stdout}${stderr}`.trim();
  } catch (error) {
    return error.stdout || error.stderr ? `${error.stdout ?? ''}${error.stderr ?? ''}`.trim() : null;
  }
}

function parseDarwinRouteInterface(output) {
  return output?.match(/interface:\s+(\S+)/)?.[1] ?? null;
}

function indent(value) {
  return value.split(/\r?\n/).map((line) => `  ${line}`).join('\n');
}
