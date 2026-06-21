import { once } from 'node:events';

export const PROTOCOL_VERSION = 1;
export const DEFAULT_PORT = 44777;
export const DEFAULT_BLOCK_SIZE = 16 * 1024 * 1024;
export const LEGACY_BLOCK_SIZE = 4 * 1024 * 1024;
export const DEFAULT_PIPELINE_WINDOW = 4;
export const MAX_HEADER_BYTES = 64 * 1024;

export async function writeFrame(socket, header, payload = Buffer.alloc(0)) {
  const headerBytes = Buffer.from(JSON.stringify(header), 'utf8');
  const payloadBytes = Buffer.isBuffer(payload) ? payload : Buffer.from(payload);

  if (headerBytes.length > MAX_HEADER_BYTES) {
    throw new Error(`Protocol header too large: ${headerBytes.length} bytes`);
  }

  const prefix = Buffer.allocUnsafe(8);
  prefix.writeUInt32BE(headerBytes.length, 0);
  prefix.writeUInt32BE(payloadBytes.length, 4);

  socket.cork();
  const canWritePrefix = socket.write(prefix);
  const canWriteHeader = socket.write(headerBytes);
  const canWritePayload = payloadBytes.length ? socket.write(payloadBytes) : true;
  socket.uncork();

  if (!canWritePrefix || !canWriteHeader || !canWritePayload) {
    await once(socket, 'drain');
  }
}

export class FrameReader {
  constructor(socket) {
    this.socket = socket;
    this.chunks = [];
    this.bufferedBytes = 0;
    this.waiters = [];
    this.done = false;
    this.error = null;

    socket.on('data', (chunk) => {
      this.chunks.push(chunk);
      this.bufferedBytes += chunk.length;
      this.#wake();
    });
    socket.on('end', () => {
      this.done = true;
      this.#wake();
    });
    socket.on('close', () => {
      this.done = true;
      this.#wake();
    });
    socket.on('error', (error) => {
      this.error = error;
      this.done = true;
      this.#wake();
    });
  }

  async readFrame() {
    for (;;) {
      if (this.error) throw this.error;

      const frame = this.#tryRead();
      if (frame) return frame;

      if (this.done) return null;
      await new Promise((resolve) => this.waiters.push(resolve));
    }
  }

  #tryRead() {
    if (this.bufferedBytes < 8) return null;

    const prefix = this.#peek(8);
    const headerLength = prefix.readUInt32BE(0);
    const payloadLength = prefix.readUInt32BE(4);
    if (headerLength > MAX_HEADER_BYTES) {
      throw new Error(`Protocol header too large: ${headerLength} bytes`);
    }

    const frameLength = 8 + headerLength + payloadLength;
    if (this.bufferedBytes < frameLength) return null;

    this.#consume(8);
    const headerBytes = this.#consume(headerLength);
    const payload = this.#consume(payloadLength);

    return {
      header: JSON.parse(headerBytes.toString('utf8')),
      payload
    };
  }

  #peek(length) {
    if (this.chunks[0].length >= length) {
      return this.chunks[0].subarray(0, length);
    }

    const output = Buffer.allocUnsafe(length);
    let offset = 0;
    for (const chunk of this.chunks) {
      const copyLength = Math.min(chunk.length, length - offset);
      chunk.copy(output, offset, 0, copyLength);
      offset += copyLength;
      if (offset === length) break;
    }
    return output;
  }

  #consume(length) {
    if (length === 0) return Buffer.alloc(0);
    if (this.chunks[0].length === length) {
      this.bufferedBytes -= length;
      return this.chunks.shift();
    }
    if (this.chunks[0].length > length) {
      const output = this.chunks[0].subarray(0, length);
      this.chunks[0] = this.chunks[0].subarray(length);
      this.bufferedBytes -= length;
      return output;
    }

    const output = Buffer.allocUnsafe(length);
    let offset = 0;
    while (offset < length) {
      const chunk = this.chunks[0];
      const copyLength = Math.min(chunk.length, length - offset);
      chunk.copy(output, offset, 0, copyLength);
      offset += copyLength;

      if (copyLength === chunk.length) {
        this.chunks.shift();
      } else {
        this.chunks[0] = chunk.subarray(copyLength);
      }
    }
    this.bufferedBytes -= length;
    return output;
  }

  #wake() {
    const waiters = this.waiters.splice(0);
    for (const waiter of waiters) waiter();
  }
}
