/**
 * Types shared by the server responder and the browser.
 *
 * Kept apart from `responder.ts` so client components can import them without
 * pulling the NATS client into the browser bundle.
 */
import type { KeyType, PermissionRequest } from "./schemas";

/**
 * Where the signing key lives. `browser` is the only signing mode: the private
 * half is a non-extractable WebCrypto key in IndexedDB, so the server can verify
 * but never sign (see `browser-key.ts`).
 */
export type SigningMode = "unsigned" | "browser";

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
  /** True once a key has been registered — says nothing about who holds it. */
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
  error?: string;
}
