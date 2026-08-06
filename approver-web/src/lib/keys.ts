/**
 * P-256 keys in the exact shape `lib/crypto.py` expects (server only).
 *
 * The wire formats are not negotiable — `registration_handler.py` stores what we
 * publish and `hook.py` verifies against it (CLAUDE.md §7, `key_type: "p256"`):
 *
 *   private key  32-byte big-endian scalar, base64
 *   public key   33-byte X9.62 *compressed* point, base64
 *   signature    DER (variable length), base64, ECDSA over SHA-256
 *
 * Node hands out JWK (`d`/`x`/`y`, base64url), so both directions need a
 * conversion. Re-importing a bare scalar is the awkward one: JWK import demands
 * `x`/`y`, and recovering them from a compressed point means a modular square
 * root. Instead the scalar is wrapped in a SEC1 `ECPrivateKey` DER structure with
 * the public key omitted — OpenSSL derives it. That keeps the stored config
 * byte-identical to `responder-config.json`, rather than inventing a second
 * private-key encoding for the web app.
 */
import { createPrivateKey, createSign, generateKeyPairSync } from "node:crypto";

/** DER prologue for SEC1 ECPrivateKey { version 1, privateKey OCTET STRING(32) }. */
const SEC1_HEAD = Buffer.from("30310201010420", "hex");
/** [0] { OID 1.2.840.10045.3.1.7 } — prime256v1. */
const SEC1_CURVE = Buffer.from("a00a06082a8648ce3d030107", "hex");

export interface GeneratedKey {
  /** base64, 32-byte scalar. */
  privateB64: string;
  /** base64, 33-byte compressed point — what registration publishes. */
  publicB64: string;
}

/** Compress an uncompressed (x, y) pair: 0x02/0x03 by y parity, then x. */
function compressPoint(x: Buffer, y: Buffer): Buffer {
  const prefix = (y[y.length - 1]! & 1) === 0 ? 0x02 : 0x03;
  return Buffer.concat([Buffer.from([prefix]), x]);
}

export function generateP256(): GeneratedKey {
  const { privateKey } = generateKeyPairSync("ec", { namedCurve: "prime256v1" });
  const jwk = privateKey.export({ format: "jwk" });
  if (!jwk.d || !jwk.x || !jwk.y) throw new Error("node produced an EC key without d/x/y");

  const d = Buffer.from(jwk.d, "base64url");
  const x = Buffer.from(jwk.x, "base64url");
  const y = Buffer.from(jwk.y, "base64url");
  if (d.length !== 32 || x.length !== 32 || y.length !== 32) {
    throw new Error("unexpected P-256 component length");
  }

  return {
    privateB64: d.toString("base64"),
    publicB64: compressPoint(x, y).toString("base64"),
  };
}

/** Rebuild a signing key from the stored 32-byte scalar. */
function privateKeyFromScalar(privateB64: string) {
  const d = Buffer.from(privateB64, "base64");
  if (d.length !== 32) throw new Error("P-256 private key must be 32 bytes");
  return createPrivateKey({
    key: Buffer.concat([SEC1_HEAD, d, SEC1_CURVE]),
    format: "der",
    type: "sec1",
  });
}

/**
 * Sign the §7 signing bytes. Node's EC signatures are DER by default, and
 * `createSign("SHA256")` hashes once — matching `crypto.verify(..., "p256")`,
 * which runs `ECDSA(SHA256)` over the same bytes. Do not pre-hash.
 */
export function signWithP256(privateB64: string, data: Uint8Array): string {
  const key = privateKeyFromScalar(privateB64);
  return createSign("SHA256").update(data).sign(key).toString("base64");
}
