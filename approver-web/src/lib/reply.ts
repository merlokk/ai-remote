/**
 * Reply assembly — a port of `responder.build_signed_reply` (CLAUDE.md §7).
 *
 * Split in two on purpose, because the two halves now run in different places:
 * the **browser** computes the signing bytes and signs them (it holds the key),
 * the **server** assembles the reply that goes on the wire. Both call the same
 * functions from this file, so there is one definition of what gets signed.
 *
 * Echoes `v/session_id/tool_name/input_sha256/nonce/ts` from the request; the
 * responder contributes `behavior/reason/updated_input`. `updated_input` is
 * honored only on allow, and its hash goes into the signature without ever
 * travelling on the wire — the hook recomputes it from the object it received.
 */
import { canonicalSha256, signingBytes } from "./protocol";
import type { Behavior, PermissionRequest } from "./schemas";

export interface SignedReply {
  v: number;
  behavior: Behavior;
  reason: string;
  session_id: string;
  tool_name: string;
  input_sha256: string;
  nonce: string;
  ts: number;
  key_id: string;
  sig: string;
  updated_input?: Record<string, unknown>;
}

export interface Decision {
  behavior: Behavior;
  reason: string;
  updatedInput: Record<string, unknown> | null;
}

/** `updated_input` counts only on allow — on deny it is ignored, hash included. */
export function appliesUpdate(decision: Decision): boolean {
  return decision.behavior === "allow" && decision.updatedInput != null;
}

/** The exact bytes to sign for this decision. Identical on both sides. */
export async function decisionSigningBytes(
  request: PermissionRequest,
  decision: Decision,
): Promise<Uint8Array> {
  const updatedInputSha256 = appliesUpdate(decision)
    ? await canonicalSha256(decision.updatedInput)
    : "";
  return signingBytes({
    v: request.v,
    session_id: request.session_id,
    nonce: request.nonce,
    tool_name: request.tool_name,
    input_sha256: request.input_sha256,
    behavior: decision.behavior,
    updated_input_sha256: updatedInputSha256,
    ts: request.ts,
    reason: decision.reason,
  });
}

/** Build the wire reply around a signature produced elsewhere. */
export function assembleReply(
  request: PermissionRequest,
  decision: Decision,
  { keyId, sig }: { keyId: string; sig: string },
): SignedReply {
  const reply: SignedReply = {
    v: request.v,
    behavior: decision.behavior,
    reason: decision.reason,
    session_id: request.session_id,
    tool_name: request.tool_name,
    input_sha256: request.input_sha256,
    nonce: request.nonce,
    ts: request.ts,
    key_id: keyId,
    sig,
  };
  if (appliesUpdate(decision)) reply.updated_input = decision.updatedInput!;
  return reply;
}
