/**
 * Server-side verification of what the browser signed.
 *
 * The server holds **no private key** any more — custody moved into the browser
 * (`browser-key.ts`). What it keeps is the registered public key, and it uses it
 * to check each signature before answering the bus.
 *
 * That check is not ceremony. `approver/responder_yubikey.py`'s `device_signer`
 * self-verifies for the same reason: a signature the registered key cannot check
 * is useless, and this is the only place where the failure can still be
 * explained to the operator instead of surfacing as an unexplained fall-back to
 * Claude Code's own prompt.
 *
 * Verification needs the **uncompressed** point (JWK wants `x` and `y`), while
 * the allowlist holds the **compressed** one; recovering y from x is a modular
 * square root. So registration stores both — the browser exports the
 * uncompressed form anyway.
 */
import { createPublicKey, verify as nodeVerify } from "node:crypto";

/** Rebuild a public key from a base64 65-byte uncompressed SEC1 point. */
function publicKeyFromRaw(rawB64: string) {
  const raw = Buffer.from(rawB64, "base64");
  if (raw.length !== 65 || raw[0] !== 0x04) {
    throw new Error(`expected a 65-byte uncompressed point, got ${raw.length} bytes`);
  }
  return createPublicKey({
    key: {
      kty: "EC",
      crv: "P-256",
      x: raw.subarray(1, 33).toString("base64url"),
      y: raw.subarray(33, 65).toString("base64url"),
    },
    format: "jwk",
  });
}

/**
 * Verify a DER signature over `data`, ECDSA P-256 / SHA-256 — the same scheme
 * `lib/crypto.verify(..., "p256")` applies, hashing once.
 *
 * Fail-safe like its Python counterpart: any malformed input is `false`, never
 * an exception.
 */
export function verifyP256(publicRawB64: string, data: Uint8Array, sigB64: string): boolean {
  try {
    return nodeVerify(
      "sha256",
      data,
      publicKeyFromRaw(publicRawB64),
      Buffer.from(sigB64, "base64"),
    );
  } catch {
    return false;
  }
}

/**
 * SPKI wrapper for a bare Ed25519 public key: `SEQUENCE { AlgorithmIdentifier
 * { 1.3.101.112 }, BIT STRING }`. Fixed for every 32-byte key, so prefixing is
 * the whole conversion — `node:crypto` will not import a raw one, while the
 * §6 wire format (and `lib/crypto.py`) is base64 of exactly those 32 bytes.
 */
const ED25519_SPKI_PREFIX = Buffer.from("302a300506032b6570032100", "hex");

/**
 * Verify an Ed25519 signature — the scheme the registration handler signs its
 * replies with (`lib/crypto.verify(..., "ed25519")`).
 *
 * Ed25519 hashes internally, so `data` is the signing bytes themselves. Same
 * fail-safe contract as {@link verifyP256}: malformed input is `false`.
 */
export function verifyEd25519(publicB64: string, data: Uint8Array, sigB64: string): boolean {
  try {
    const raw = Buffer.from(publicB64, "base64");
    if (raw.length !== 32) return false;
    const key = createPublicKey({
      key: Buffer.concat([ED25519_SPKI_PREFIX, raw]),
      format: "der",
      type: "spki",
    });
    return nodeVerify(null, data, key, Buffer.from(sigB64, "base64"));
  } catch {
    return false;
  }
}
