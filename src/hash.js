import { createHash } from 'node:crypto';
import xxhash from 'xxhash-wasm';

export const DEFAULT_HASH_ALGORITHM = 'xxh64';
export const FALLBACK_HASH_ALGORITHM = 'blake2b512';
export const SUPPORTED_HASH_ALGORITHMS = [DEFAULT_HASH_ALGORITHM, FALLBACK_HASH_ALGORITHM];

let xxhashApi = null;
const xxhashApiPromise = xxhash().then((api) => {
  xxhashApi = api;
  return api;
});

export async function initHashing() {
  await xxhashApiPromise;
}

export function chooseHashAlgorithm(supported = []) {
  for (const algorithm of SUPPORTED_HASH_ALGORITHMS) {
    if (supported.includes(algorithm)) return algorithm;
  }
  return FALLBACK_HASH_ALGORITHM;
}

export function createFastHash(algorithm = DEFAULT_HASH_ALGORITHM) {
  if (algorithm === 'xxh64' && xxhashApi) {
    const hasher = xxhashApi.create64();
    return {
      update(input) {
        hasher.update(input);
        return this;
      },
      digest() {
        return toHex64(hasher.digest());
      }
    };
  }

  return createHash('blake2b512');
}

export function hashBuffer(buffer, algorithm = DEFAULT_HASH_ALGORITHM) {
  if (algorithm === 'xxh64' && xxhashApi) {
    return toHex64(xxhashApi.h64Raw(buffer));
  }

  return createHash('blake2b512').update(buffer).digest('hex');
}

function toHex64(value) {
  return value.toString(16).padStart(16, '0');
}
