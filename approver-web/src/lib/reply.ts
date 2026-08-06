/**
 * Reply assembly — a port of `responder.build_signed_reply` (CLAUDE.md §7).
 *
 * Echoes `v/session_id/tool_name/input_sha256/nonce/ts` from the request; the
 * responder contributes `behavior/reason/updated_input`. `updated_input` is
 * honored only on allow, and its hash goes into the signature without ever
 * travelling on the wire — the hook recomputes it from the object it received.
 */
import { canonicalSha256, signingBytes } from "./protocol";
import type { Behavior, PermissionRequest } from "./schemas";
import type { Signer } from "./signer";

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

export interface BuildReplyOptions {
  behavior: Behavior;
  signer: Signer;
  reason?: string;
  updatedInput?: Record<string, unknown> | null;
}

export async function buildSignedReply(
  request: PermissionRequest,
  { behavior, signer, reason = "", updatedInput = null }: BuildReplyOptions,
): Promise<SignedReply> {
  const applyUpdate = behavior === "allow" && updatedInput != null;
  const updatedInputSha256 = applyUpdate ? await canonicalSha256(updatedInput) : "";

  const sig = await signer.sign(
    signingBytes({
      v: request.v,
      session_id: request.session_id,
      nonce: request.nonce,
      tool_name: request.tool_name,
      input_sha256: request.input_sha256,
      behavior,
      updated_input_sha256: updatedInputSha256,
      ts: request.ts,
      reason,
    }),
  );

  const reply: SignedReply = {
    v: request.v,
    behavior,
    reason,
    session_id: request.session_id,
    tool_name: request.tool_name,
    input_sha256: request.input_sha256,
    nonce: request.nonce,
    ts: request.ts,
    key_id: signer.keyId,
    sig,
  };
  if (applyUpdate) reply.updated_input = updatedInput;
  return reply;
}
