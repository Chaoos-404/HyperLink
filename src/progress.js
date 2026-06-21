export class Progress {
  constructor({ label = 'transfer', enabled = process.stderr.isTTY } = {}) {
    this.label = label;
    this.enabled = enabled;
    this.reset();
  }

  reset() {
    this.startedAt = Date.now();
    this.lastPaint = 0;
    this.bytes = 0;
    this.file = '';
  }

  setFile(file) {
    this.file = file;
    this.paint(true);
  }

  add(bytes) {
    this.bytes += bytes;
    this.paint(false);
  }

  paint(force) {
    if (!this.enabled) return;
    const now = Date.now();
    if (!force && now - this.lastPaint < 150) return;
    this.lastPaint = now;
    const seconds = Math.max((now - this.startedAt) / 1000, 0.001);
    const rate = this.bytes / seconds;
    const line = `${this.label}: ${formatBytes(this.bytes)} at ${formatBytes(rate)}/s ${this.file ? `| ${this.file}` : ''}`;
    process.stderr.write(`\r${line.padEnd(process.stderr.columns ?? 100)}`);
  }

  done() {
    if (!this.enabled) return;
    this.paint(true);
    process.stderr.write('\n');
  }
}

export function formatBytes(bytes) {
  const units = ['B', 'KB', 'MB', 'GB', 'TB'];
  let value = bytes;
  let unit = 0;
  while (value >= 1024 && unit < units.length - 1) {
    value /= 1024;
    unit += 1;
  }
  return `${value.toFixed(unit === 0 ? 0 : 1)} ${units[unit]}`;
}
