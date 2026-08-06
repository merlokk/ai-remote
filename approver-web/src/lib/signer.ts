/**
 * The signing seam — the phase-1 / phase-2 boundary.
 *
 * `approver/responder.py` already models a signer as a `sign(bytes) -> b64`
 * callable (`build_signed_reply(..., sign=)`), which is how the YubiKey
 * responder plugs a device in without duplicating any protocol code. This is the
 * same seam in TypeScript, so phase 2 replaces one object and nothing else.
 *
 * Phase 1 ships `unsignedSigner`: it returns an empty signature, and
 * `hook.py` rejects it — untrusted reply, exit != 0, Claude Code falls back to
 * its own prompt (§7 fail-safe). That is the intended behavior for now: the
 * round trip is fully observable while no key exists, and there is no code path
 * by which a missing signature could become an allow.
 */
import type { KeyType } from "./schemas";

export type SigningMode = "unsigned" | "software" | "webcrypto";

export interface Signer {
  keyId: string;
  keyType: KeyType;
  mode: SigningMode;
  /** Returns the base64 signature over `bytes`, or "" when it cannot sign. */
  sign(bytes: Uint8Array): Promise<string>;
}

export function unsignedSigner(keyId: string, keyType: KeyType): Signer {
  return {
    keyId,
    keyType,
    mode: "unsigned",
    async sign(_bytes: Uint8Array): Promise<string> {
      return "";
    },
  };
}
