/**
 * Wire-protocol primitives — a port of `approver/protocol.py` (CLAUDE.md §7).
 *
 * These bytes are verified by `hook.py`, so this file must agree with the Python
 * implementation exactly. Two details do the damage if you get them wrong:
 *
 *  1. `json.dumps(..., sort_keys=True, separators=(",", ":"))` defaults to
 *     `ensure_ascii=True`, so Python escapes every non-ASCII character as
 *     `\uXXXX` (lowercase hex). `JSON.stringify` emits it literally. We
 *     re-escape to match — see {@link canonicalJson}.
 *  2. The signing-bytes field order and the `\n` separator are the contract;
 *     `reason` is last because it is the only field that may contain newlines.
 *
 * Isomorphic on purpose: hashing goes through Web Crypto (present in both Node
 * and the browser) so phase 2 can sign in the browser with the same module.
 */

export const PROTOCOL_VERSION = 1;

/**
 * The registration handler's own key scheme (`approver/CLAUDE.md` §6 "server key").
 * Fixed, never taken from a reply: this app pins one key *and* one algorithm.
 */
export const SERVER_KEY_TYPE = "ed25519";

/** Domain separator on every registration-reply signature — see {@link registrationReplySigningBytes}. */
export const REGISTRATION_REPLY_CONTEXT = "registration-reply";

/** Matches every code unit Python's ensure_ascii=True would escape. */
const NON_ASCII = /[\u0080-\uffff]/g;

/** Recursively sort object keys; arrays keep their order, scalars pass through. */
function sortKeysDeep(value: unknown): unknown {
  if (Array.isArray(value)) return value.map(sortKeysDeep);
  if (value !== null && typeof value === "object") {
    const src = value as Record<string, unknown>;
    const out: Record<string, unknown> = {};
    for (const key of Object.keys(src).sort()) out[key] = sortKeysDeep(src[key]);
    return out;
  }
  return value;
}

/**
 * Canonical JSON for hashing: sorted keys, no whitespace, non-ASCII escaped.
 *
 * The escape pass is what makes this match Python's `ensure_ascii=True`. Lone
 * UTF-16 code units are escaped individually, which is also what Python does
 * with astral characters (it emits a surrogate pair).
 */
export function canonicalJson(value: unknown): string {
  return JSON.stringify(sortKeysDeep(value)).replace(
    NON_ASCII,
    (ch) => "\\u" + ch.charCodeAt(0).toString(16).padStart(4, "0"),
  );
}

function toHex(bytes: ArrayBuffer): string {
  return Array.from(new Uint8Array(bytes))
    .map((b) => b.toString(16).padStart(2, "0"))
    .join("");
}

export async function sha256Hex(data: Uint8Array): Promise<string> {
  // Copy into a fresh ArrayBuffer: a Uint8Array view may be a slice of a larger
  // buffer, and digest() would hash the whole thing.
  const digest = await crypto.subtle.digest("SHA-256", new Uint8Array(data).buffer);
  return toHex(digest);
}

/** Hex sha256 of the canonical JSON encoding of `value`. */
export async function canonicalSha256(value: unknown): Promise<string> {
  return sha256Hex(new TextEncoder().encode(canonicalJson(value)));
}

export interface SigningFields {
  v: number;
  session_id: string;
  nonce: string;
  tool_name: string;
  input_sha256: string;
  behavior: string;
  updated_input_sha256: string;
  ts: number;
  reason: string;
}

/**
 * Assemble the exact bytes that get signed/verified (§7 "Signing bytes").
 *
 * Layout (`\n`-joined, utf-8): v, session_id, nonce, tool_name, input_sha256,
 * behavior, updated_input_sha256, ts, reason.
 */
export function signingBytes(f: SigningFields): Uint8Array {
  const parts = [
    String(f.v),
    f.session_id,
    f.nonce,
    f.tool_name,
    f.input_sha256,
    f.behavior,
    f.updated_input_sha256,
    String(f.ts),
    f.reason,
  ];
  return new TextEncoder().encode(parts.join("\n"));
}

export interface RegistrationReplyFields {
  v: number;
  ok: boolean;
  key_id: string;
  nonce: string;
  ts: number;
  error: string;
}

/**
 * The bytes the registration handler signs over its reply — a port of
 * `protocol.registration_reply_signing_bytes` (§6).
 *
 * Layout (`\n`-joined, utf-8): the context string, v, ok (`"true"`/`"false"`),
 * key_id, nonce, ts, error. The nonce is the one this app sent, so a reply is
 * good for exactly one exchange; `error` is last because it is the free-text
 * field.
 */
export function registrationReplySigningBytes(f: RegistrationReplyFields): Uint8Array {
  const parts = [
    REGISTRATION_REPLY_CONTEXT,
    String(f.v),
    f.ok ? "true" : "false",
    f.key_id,
    f.nonce,
    String(f.ts),
    f.error,
  ];
  return new TextEncoder().encode(parts.join("\n"));
}
