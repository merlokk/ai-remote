/**
 * Types shared by the server responder and the browser.
 *
 * Kept apart from `responder.ts` so client components can import them without
 * pulling the NATS client into the browser bundle.
 */
import type { ActivityDoc } from "./activity";
import type { KeyType, PermissionRequest } from "./schemas";
import type { StatusDoc } from "./statusline";

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

/**
 * One registered browser as the page sees it: **public material only**, which is
 * all this app ever has (custody is in the browser — `browser-key.ts`).
 *
 * There can be several, one per `key_id`, which is what lets two operators each
 * hold their own key. A browser finds itself in this list by its own `key_id`,
 * and compares `public_key` to know whether the key it holds is still the
 * registered one.
 */
export interface RegisteredClientView {
  key_id: string;
  key_type: KeyType;
  /** base64, 33-byte compressed point — what the allowlist holds. */
  public_key: string;
  /** Epoch seconds, or null for an entry migrated from the pre-`clients` shape. */
  registered_ts: number | null;
}

export interface ResponderStatus {
  connected: boolean;
  servers: string;
  subject: string;
  queue: string;
  /** The status line's subject (§9.7) — watched, never answered. */
  status_subject: string;
  /** The activity subject (§9.10) — watched the same way, and just as read-only. */
  activity_subject: string;
  /** Every registered browser, sorted by `key_id`. Empty until one registers. */
  clients: RegisteredClientView[];
  signing_mode: SigningMode;
  /** True once *some* key is registered — says nothing about who holds it. */
  registered: boolean;
  /**
   * The registration handler's public key, pinned when this app registered
   * (§6). Shown so it can be compared with what the handler printed — that
   * comparison is the only check on the first, trust-on-first-use registration.
   */
  server_key: string | null;
  config_path: string;
  config_from_file: boolean;
  /** Last connection/subscription error, for the status bar. */
  error: string | null;
}

export interface Snapshot {
  status: ResponderStatus;
  requests: PendingRequest[];
  /**
   * The last document seen on the status line's subject (§9.7), or null while
   * nothing has been published since this process started.
   *
   * A *current value*, not a queue: the newest message wins and nothing is kept
   * behind it. Every Claude Code session on the machine publishes to the same
   * subject, so with two of them running this is whichever rendered last — which
   * is why the plaque shows the document's `cwd` and its age rather than implying
   * it belongs to the request above it.
   */
  statusline: StatusDoc | null;
  /**
   * The last document seen on the activity subject (§9.10), or null while
   * nothing has been published since this process started.
   *
   * A current value like `statusline`, and kept for the same reason: the newest
   * message wins, nothing is queued behind it, and a session that has gone
   * quiet leaves its last state on screen with its age beside it rather than
   * blanking.
   */
  activity: ActivityDoc | null;
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
