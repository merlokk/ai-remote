/**
 * The responder — server side only (holds the NATS connection).
 *
 * A browser cannot open a TCP connection to :4222, so the Next.js server keeps
 * the connection and the browser drives it over SSE + POST. See CLAUDE.md in
 * this folder for why that beats `nats.ws` here.
 *
 * One process-wide instance, stashed on `globalThis` so the dev server's hot
 * reload does not leave a second subscription in the `approvers` queue group.
 *
 * Behavior deliberately mirrored from the Python responders:
 *   - a message that is not a valid §7 request is dropped with a warning, never
 *     answered and never crashes the loop (`lib/bus.py`);
 *   - no decision means no reply — the hook times out and Claude Code shows its
 *     own prompt. Silence is the safe outcome, so expiry just forgets the entry.
 */
import {
  connect,
  type Msg,
  type NatsConnection,
  type Subscription,
} from "@nats-io/transport-node";

import { randomBytes } from "node:crypto";

import { type ActivityDoc, activityDocSchema } from "./activity";
import { loadConfig, saveConfig } from "./config";
import { verifyEd25519, verifyP256 } from "./keys";
import { PROTOCOL_VERSION, registrationReplySigningBytes } from "./protocol";
import {
  assembleReply,
  type Decision,
  decisionSigningBytes,
  type SignedReply,
} from "./reply";
import {
  permissionRequestSchema,
  type RegistrationReply,
  registrationReplySchema,
} from "./schemas";
import { type StatusDoc, statusDocSchema } from "./statusline";
import type { PendingRequest, ResponderStatus, SigningMode, Snapshot } from "./types";

const decoder = new TextDecoder();
const encoder = new TextEncoder();

interface Entry {
  pending: PendingRequest;
  /** Held so we can answer into the request's reply inbox later. */
  msg: Msg;
}

export class DecisionError extends Error {}
/** The handler said no, or the bus did — nothing was persisted. */
export class RegistrationError extends Error {}

const REGISTRATION_SUBJECT = "registrations";
const REGISTRATION_TIMEOUT_MS = 10_000;
const NONCE_BYTES = 32;

/**
 * Check the registration handler's signature over its reply; return its key.
 *
 * A port of `responder.verify_server_reply` (`approver/CLAUDE.md` §6), and the reason
 * this app can believe an answer at all: `registrations` is an open subject, so
 * without this any client on the bus could ack a registration — or, worse, deny
 * one and be believed.
 *
 * `pinned` is the key already in `config.json`. When present the reply must be
 * signed by exactly it: a good signature by a *different* key is precisely the
 * takeover the pin exists for. On the first registration there is nothing to
 * pin against, so the key is taken on trust and pinned from then on — compare
 * it once with what the handler printed when it started.
 */
function verifyRegistrationReply(
  reply: RegistrationReply,
  { nonce, pinned }: { nonce: string; pinned?: string },
): string {
  if (reply.v !== PROTOCOL_VERSION) throw new RegistrationError("unexpected protocol version");

  const serverKey = reply.server_key;
  if (!serverKey) {
    throw new RegistrationError(
      "the reply carries no server key — is the registration handler up to date?",
    );
  }
  if (pinned && serverKey !== pinned) {
    throw new RegistrationError(
      "the reply is signed by a different key than the registration handler this app " +
        "already trusts — refusing to re-pin it",
    );
  }
  if (reply.nonce !== nonce) {
    throw new RegistrationError("the reply does not match this request (a replayed reply?)");
  }
  if (typeof reply.ts !== "number") throw new RegistrationError("the reply has no timestamp");

  const bytes = registrationReplySigningBytes({
    v: PROTOCOL_VERSION,
    ok: reply.ok,
    key_id: reply.key_id ?? "",
    nonce,
    ts: reply.ts,
    error: reply.error ?? "",
  });
  if (!reply.sig || !verifyEd25519(serverKey, bytes, reply.sig)) {
    throw new RegistrationError("the registration handler's signature does not verify");
  }
  return serverKey;
}

class Responder {
  private entries = new Map<string, Entry>();
  private listeners = new Set<(snapshot: Snapshot) => void>();
  private nc: NatsConnection | null = null;
  private sub: Subscription | null = null;
  /** The status line's subject (§9.7) — watched for the plaque, never answered. */
  private statusSub: Subscription | null = null;
  private statusDoc: StatusDoc | null = null;
  /** The activity subject (§9.10) — the same arrangement, one row lower. */
  private activitySub: Subscription | null = null;
  private activityDoc: ActivityDoc | null = null;
  private starting: Promise<void> | null = null;
  private sweeper: ReturnType<typeof setInterval> | null = null;
  private error: string | null = null;

  private loaded = loadConfig();

  private get signingMode(): SigningMode {
    return this.loaded.config.public_key_raw ? "browser" : "unsigned";
  }

  get config() {
    return this.loaded.config;
  }

  /** Idempotent: safe to call on every SSE connect. */
  start(): Promise<void> {
    if (this.starting) return this.starting;
    this.starting = this.connectAndSubscribe().catch((err: unknown) => {
      this.error = err instanceof Error ? err.message : String(err);
      this.starting = null; // let the next viewer retry
      this.notify();
    });
    return this.starting;
  }

  private async connectAndSubscribe(): Promise<void> {
    const { servers, subject, queue, status_subject, activity_subject } = this.config;
    const nc = await connect({ servers, name: "approver-web" });
    this.nc = nc;
    this.error = null;

    this.sub = nc.subscribe(subject, { queue });
    void this.consume(this.sub);

    // No queue group, unlike the approvals subscription above: a decision must be
    // made once, but the status line is a broadcast of a current value and every
    // subscriber is meant to see every message.
    this.statusSub = nc.subscribe(status_subject);
    void this.consumeStatus(this.statusSub);

    // Same again for §9.10, and with no queue group for the same reason.
    this.activitySub = nc.subscribe(activity_subject);
    void this.consumeActivity(this.activitySub);

    void nc.closed().then((err) => {
      this.nc = null;
      this.sub = null;
      this.statusSub = null;
      this.activitySub = null;
      this.starting = null;
      if (err) this.error = err.message;
      // The last document is deliberately kept: it is stamped with its own clock
      // and shown with its age, so it degrades into history rather than vanishing
      // because the bus blinked.
      this.notify();
    });

    this.sweeper ??= setInterval(() => this.sweep(), 1000);
    this.sweeper.unref?.();
    this.notify();
  }

  private async consume(sub: Subscription): Promise<void> {
    for await (const msg of sub) {
      try {
        this.accept(msg);
      } catch (err) {
        console.warn("[approver-web] dropped a message:", err);
      }
    }
  }

  /**
   * The status line's documents (`statusline/CLAUDE.md` §9.7) — the model, the 5h
   * and 7d rate limits, the context window.
   *
   * Nothing here touches the approval flow: it is a read-only feed for the plaque
   * on the page, it is never answered, and a subject with no publisher at all just
   * means the plaque stays empty. Dropped like a malformed request, and for the
   * same reason (`status` is as open as `approvals.*`): a stray `nats pub status
   * hello` must not blank a readout that was correct a second ago.
   */
  private async consumeStatus(sub: Subscription): Promise<void> {
    for await (const msg of sub) {
      let payload: unknown;
      try {
        payload = JSON.parse(decoder.decode(msg.data));
      } catch {
        console.warn("[approver-web] ignoring a non-JSON message on", msg.subject);
        continue;
      }

      const parsed = statusDocSchema.safeParse(payload);
      if (!parsed.success) {
        console.warn("[approver-web] ignoring a malformed status document:", parsed.error.message);
        continue;
      }

      this.statusDoc = parsed.data;
      this.notify();
    }
  }

  /**
   * What Claude is doing (`statusline/CLAUDE.md` §9.10) — the tool about to
   * run, the tool that just ran, the turn that ended.
   *
   * The same posture as `consumeStatus` above, for the same reasons: read-only,
   * never answered, and a malformed message dropped rather than allowed to blank
   * a row that was correct a second ago. The schema requires `v`, which on an
   * open subject is what tells one of these documents from anything else.
   */
  private async consumeActivity(sub: Subscription): Promise<void> {
    for await (const msg of sub) {
      let payload: unknown;
      try {
        payload = JSON.parse(decoder.decode(msg.data));
      } catch {
        console.warn("[approver-web] ignoring a non-JSON message on", msg.subject);
        continue;
      }

      const parsed = activityDocSchema.safeParse(payload);
      if (!parsed.success) {
        console.warn(
          "[approver-web] ignoring a malformed activity document:",
          parsed.error.message,
        );
        continue;
      }

      this.activityDoc = parsed.data;
      this.notify();
    }
  }

  private accept(msg: Msg): void {
    if (!msg.reply) {
      // Fire-and-forget publish on an open subject: nobody is waiting for us.
      console.warn("[approver-web] ignoring a message with no reply inbox");
      return;
    }

    let payload: unknown;
    try {
      payload = JSON.parse(decoder.decode(msg.data));
    } catch {
      console.warn("[approver-web] ignoring a non-JSON message on", msg.subject);
      return;
    }

    const parsed = permissionRequestSchema.safeParse(payload);
    if (!parsed.success) {
      console.warn("[approver-web] ignoring a malformed request:", parsed.error.message);
      return;
    }

    const now = Date.now();
    this.entries.set(parsed.data.nonce, {
      msg,
      pending: {
        nonce: parsed.data.nonce,
        request: parsed.data,
        received_at: now,
        expires_at: now + this.config.request_ttl * 1000,
      },
    });
    this.notify();
  }

  private sweep(): void {
    const now = Date.now();
    let dropped = false;
    for (const [nonce, entry] of this.entries) {
      if (entry.pending.expires_at <= now) {
        this.entries.delete(nonce);
        dropped = true;
      }
    }
    if (dropped) this.notify();
  }

  /**
   * Answer a request with a decision the browser signed.
   *
   * The server re-derives the signing bytes from the *pending request it holds*
   * plus the posted decision, and verifies the signature against the registered
   * key before anything goes on the wire. So a caller cannot post one decision
   * and a signature over another, and a canonicalisation mismatch is caught here
   * — with a message — instead of surfacing as the hook silently falling back.
   */
  async decide(nonce: string, decision: Decision, sig: string): Promise<SignedReply> {
    const entry = this.entries.get(nonce);
    if (!entry) {
      throw new DecisionError("this request is gone (expired, or already answered)");
    }
    const { key_id, public_key_raw } = this.loaded.config;
    if (!public_key_raw) {
      throw new DecisionError("no key is registered — register this browser first");
    }

    const bytes = await decisionSigningBytes(entry.pending.request, decision);
    if (!verifyP256(public_key_raw, bytes, sig)) {
      throw new DecisionError(
        "the signature does not match the registered key — is this browser the one that registered?",
      );
    }

    const reply = assembleReply(entry.pending.request, decision, { keyId: key_id, sig });
    entry.msg.respond(encoder.encode(JSON.stringify(reply)));
    this.entries.delete(nonce);
    this.notify();
    return reply;
  }

  /**
   * Register (or rotate) the browser's key with a one-time token (§6).
   *
   * The key pair itself is generated in the browser; only the public halves
   * arrive here. Order mirrors both Python responders: publish the public key →
   * verify the handler's signature over the answer → **persist only on
   * `ok:true`**. A rejected registration must not clobber a key that still
   * works, and an unsigned answer is not a registration at all.
   */
  async register(token: string, publicB64: string, publicRawB64: string): Promise<string> {
    const keyId = token.split(".", 1)[0];
    if (!keyId) throw new RegistrationError("token has an empty key_id");

    await this.start();
    const nc = this.nc;
    if (!nc || nc.isClosed()) {
      throw new RegistrationError(this.error ?? "not connected to NATS");
    }

    // Echoed inside the handler's signature, so its reply is good for this
    // exchange only.
    const nonce = randomBytes(NONCE_BYTES).toString("base64");
    const request = {
      v: PROTOCOL_VERSION,
      token,
      key_id: keyId,
      pubkey: publicB64,
      key_type: "p256" as const,
      nonce,
      ts: Math.floor(Date.now() / 1000),
    };

    let payload: unknown;
    try {
      const msg = await nc.request(
        REGISTRATION_SUBJECT,
        encoder.encode(JSON.stringify(request)),
        { timeout: REGISTRATION_TIMEOUT_MS },
      );
      payload = JSON.parse(decoder.decode(msg.data));
    } catch (err) {
      // No responders / timeout / unparseable answer — all mean "not registered".
      const message = err instanceof Error ? err.message : String(err);
      throw new RegistrationError(
        `no answer from the registration handler (${message}) — is it running?`,
      );
    }

    const reply = registrationReplySchema.safeParse(payload);
    if (!reply.success) throw new RegistrationError("the handler sent a reply we cannot read");

    // Before the verdict is read, not after: an unsigned "rejected" is worth no
    // more than an unsigned "ok".
    const serverKey = verifyRegistrationReply(reply.data, {
      nonce,
      pinned: this.loaded.config.server_key,
    });
    if (!reply.data.ok) throw new RegistrationError(reply.data.error ?? "registration rejected");

    this.loaded.config = {
      ...this.loaded.config,
      key_id: keyId,
      key_type: "p256",
      public_key: publicB64,
      public_key_raw: publicRawB64,
      server_key: serverKey,
    };
    saveConfig(this.loaded.config, this.loaded.path);
    this.loaded.fromFile = true;
    this.notify();
    return keyId;
  }

  status(): ResponderStatus {
    return {
      connected: this.nc !== null && !this.nc.isClosed(),
      servers: this.config.servers,
      subject: this.config.subject,
      queue: this.config.queue,
      status_subject: this.config.status_subject,
      activity_subject: this.config.activity_subject,
      key_id: this.loaded.config.key_id,
      key_type: this.loaded.config.key_type,
      signing_mode: this.signingMode,
      registered: this.signingMode !== "unsigned",
      public_key: this.loaded.config.public_key ?? null,
      server_key: this.loaded.config.server_key ?? null,
      config_path: this.loaded.path,
      config_from_file: this.loaded.fromFile,
      error: this.error,
    };
  }

  snapshot(): Snapshot {
    return {
      status: this.status(),
      requests: [...this.entries.values()]
        .map((e) => e.pending)
        .sort((a, b) => a.received_at - b.received_at),
      statusline: this.statusDoc,
      activity: this.activityDoc,
    };
  }

  subscribe(listener: (snapshot: Snapshot) => void): () => void {
    this.listeners.add(listener);
    return () => this.listeners.delete(listener);
  }

  private notify(): void {
    const snapshot = this.snapshot();
    for (const listener of this.listeners) {
      try {
        listener(snapshot);
      } catch (err) {
        console.warn("[approver-web] a stream listener threw:", err);
      }
    }
  }
}

const globalRef = globalThis as typeof globalThis & { __approverWeb?: Responder };

export function getResponder(): Responder {
  globalRef.__approverWeb ??= new Responder();
  return globalRef.__approverWeb;
}
