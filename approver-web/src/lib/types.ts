/**
 * Types shared by the server responder and the browser.
 *
 * Kept apart from `responder.ts` so client components can import them without
 * pulling the `nats` module into the browser bundle.
 */
import type { KeyType, PermissionRequest } from "./schemas";
import type { SigningMode } from "./signer";

export interface PendingRequest {
  /** The request's own nonce — unique per request, so it is the entry's id. */
  nonce: string;
  request: PermissionRequest;
  /** Epoch ms, for the UI clock. */
  received_at: number;
  /** Epoch ms after which the card disappears (no reply is ever sent). */
  expires_at: number;
}

export interface ResponderStatus {
  connected: boolean;
  servers: string;
  subject: string;
  queue: string;
  key_id: string;
  key_type: KeyType;
  signing_mode: SigningMode;
  /** True once a key has been registered and decisions are actually signed. */
  registered: boolean;
  /** The registered public key (base64 compressed point), or null. */
  public_key: string | null;
  config_path: string;
  config_from_file: boolean;
  /** Last connection/subscription error, for the status bar. */
  error: string | null;
}

export interface Snapshot {
  status: ResponderStatus;
  requests: PendingRequest[];
}

export interface RegisterResult {
  ok: boolean;
  key_id?: string;
  error?: string;
}

export interface DecisionResult {
  ok: boolean;
  /** False while phase 1 replies with an empty signature (the hook rejects it). */
  signed?: boolean;
  error?: string;
}
