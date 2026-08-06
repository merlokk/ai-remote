"use client";

/**
 * The responder's signing key, held by the browser (client only).
 *
 * Custody: `generateKey(..., extractable = false, ...)` means the private half
 * can never be exported — not by this code, not by anything running on the page.
 * The `CryptoKey` object itself is put in IndexedDB, which structured-clones it
 * and keeps the non-extractable flag, so the key **survives closing the browser**
 * while never existing as bytes anywhere. Nothing signs but this tab, and the
 * Next server — which sees every request and answers the bus — cannot forge a
 * decision. That is the whole point of moving custody here.
 *
 * (Clearing site data destroys the key. That is the same as losing
 * `responder-config.json`: re-register with a fresh token.)
 *
 * Two conversions are needed because `lib/crypto.py` and WebCrypto disagree on
 * encodings — see `toDer` and `compressPoint`.
 */

const DB_NAME = "approver-web";
const DB_VERSION = 1;
const STORE = "keys";
/** Single-key store: one responder identity per browser profile. */
const RECORD_ID = "responder";

export interface StoredKey {
  id: string;
  key_id: string;
  /** base64, 33-byte compressed point — what the allowlist holds. */
  public_key: string;
  /** Non-extractable. Structured-cloned into IndexedDB as-is. */
  privateKey: CryptoKey;
  created_ts: number;
}

function openDb(): Promise<IDBDatabase> {
  return new Promise((resolve, reject) => {
    const request = indexedDB.open(DB_NAME, DB_VERSION);
    request.onupgradeneeded = () => {
      const db = request.result;
      if (!db.objectStoreNames.contains(STORE)) db.createObjectStore(STORE, { keyPath: "id" });
    };
    request.onsuccess = () => resolve(request.result);
    request.onerror = () => reject(request.error ?? new Error("cannot open IndexedDB"));
  });
}

function tx<T>(mode: IDBTransactionMode, run: (store: IDBObjectStore) => IDBRequest<T>): Promise<T> {
  return openDb().then(
    (db) =>
      new Promise<T>((resolve, reject) => {
        const request = run(db.transaction(STORE, mode).objectStore(STORE));
        request.onsuccess = () => resolve(request.result);
        request.onerror = () => reject(request.error ?? new Error("IndexedDB request failed"));
      }),
  );
}

export async function loadStoredKey(): Promise<StoredKey | null> {
  try {
    const record = await tx<StoredKey | undefined>("readonly", (s) => s.get(RECORD_ID));
    return record ?? null;
  } catch {
    // Private mode, blocked storage, a corrupt database: behave as "no key".
    return null;
  }
}

export async function storeKey(record: Omit<StoredKey, "id">): Promise<void> {
  await tx("readwrite", (s) => s.put({ ...record, id: RECORD_ID }));
}

export async function clearStoredKey(): Promise<void> {
  try {
    await tx("readwrite", (s) => s.delete(RECORD_ID));
  } catch {
    /* nothing to clean up */
  }
}

// --- encodings ------------------------------------------------------------------
function b64(bytes: Uint8Array): string {
  let binary = "";
  for (const byte of bytes) binary += String.fromCharCode(byte);
  return btoa(binary);
}

/**
 * Compress an uncompressed SEC1 point (0x04 ‖ x ‖ y, 65 bytes) to the 33-byte
 * form `lib/crypto.py` registers and verifies against: 0x02/0x03 by y parity,
 * then x. WebCrypto only exports the uncompressed one.
 */
export function compressPoint(raw: Uint8Array): Uint8Array {
  if (raw.length !== 65 || raw[0] !== 0x04) {
    throw new Error(`expected a 65-byte uncompressed point, got ${raw.length} bytes`);
  }
  const x = raw.slice(1, 33);
  const yIsOdd = (raw[64]! & 1) === 1;
  return new Uint8Array([yIsOdd ? 0x03 : 0x02, ...x]);
}

/** DER-encode one ECDSA integer: strip leading zeros, re-pad if the top bit is set. */
function derInteger(value: Uint8Array): number[] {
  let i = 0;
  while (i < value.length - 1 && value[i] === 0) i++;
  const trimmed = Array.from(value.slice(i));
  if ((trimmed[0]! & 0x80) !== 0) trimmed.unshift(0x00);
  return [0x02, trimmed.length, ...trimmed];
}

/**
 * WebCrypto ECDSA emits raw `r ‖ s` (64 bytes, fixed). `lib/crypto.py` verifies
 * DER (`SEQUENCE { r, s }`, variable length). Convert, or every signature is
 * rejected with no explanation beyond "signature verification failed".
 */
export function toDer(rawSignature: Uint8Array): Uint8Array {
  if (rawSignature.length !== 64) {
    throw new Error(`expected a 64-byte r||s signature, got ${rawSignature.length}`);
  }
  const body = [
    ...derInteger(rawSignature.slice(0, 32)),
    ...derInteger(rawSignature.slice(32, 64)),
  ];
  return new Uint8Array([0x30, body.length, ...body]);
}

// --- the key --------------------------------------------------------------------
export interface FreshKey {
  privateKey: CryptoKey;
  /** base64, 33-byte compressed point — goes into the registration request. */
  publicB64: string;
  /**
   * base64, 65-byte uncompressed point — the server keeps this to verify what we
   * sign. Returned here rather than derived later on purpose: WebCrypto's `raw`
   * import does not accept compressed points, so this is the only moment both
   * forms are available.
   */
  publicRawB64: string;
}

/**
 * A new P-256 pair whose private half is **non-extractable**.
 *
 * `extractable: false` applies to the private key; per the WebCrypto spec the
 * public key of a generated pair is always exportable, which is what makes the
 * two lines below work.
 */
export async function generateKey(): Promise<FreshKey> {
  const pair = await crypto.subtle.generateKey({ name: "ECDSA", namedCurve: "P-256" }, false, [
    "sign",
    "verify",
  ]);
  const raw = new Uint8Array(await crypto.subtle.exportKey("raw", pair.publicKey));
  return {
    privateKey: pair.privateKey,
    publicB64: b64(compressPoint(raw)),
    publicRawB64: b64(raw),
  };
}

/**
 * Sign the §7 signing bytes. One hash here (`SHA-256` inside WebCrypto) matches
 * `crypto.verify(..., "p256")`, which runs `ECDSA(SHA256)` over the same bytes.
 * Do not pre-hash.
 */
export async function signBytes(privateKey: CryptoKey, data: Uint8Array): Promise<string> {
  // Copy into a plain ArrayBuffer: a Uint8Array view may sit on a SharedArrayBuffer,
  // which BufferSource does not accept.
  const buffer = new Uint8Array(data).buffer as ArrayBuffer;
  const raw = new Uint8Array(
    await crypto.subtle.sign({ name: "ECDSA", hash: "SHA-256" }, privateKey, buffer),
  );
  return b64(toDer(raw));
}
