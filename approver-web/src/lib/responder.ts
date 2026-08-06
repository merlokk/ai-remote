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

import { loadConfig, saveConfig } from "./config";
import { generateP256 } from "./keys";
import { PROTOCOL_VERSION } from "./protocol";
import { buildSignedReply, type SignedReply } from "./reply";
import { type Behavior, permissionRequestSchema, registrationReplySchema } from "./schemas";
import { type Signer, softwareSigner, unsignedSigner } from "./signer";
import type { PendingRequest, ResponderStatus, Snapshot } from "./types";

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

class Responder {
  private entries = new Map<string, Entry>();
  private listeners = new Set<(snapshot: Snapshot) => void>();
  private nc: NatsConnection | null = null;
  private sub: Subscription | null = null;
  private starting: Promise<void> | null = null;
  private sweeper: ReturnType<typeof setInterval> | null = null;
  private error: string | null = null;

  private loaded = loadConfig();
  private signer: Signer = this.signerFromConfig();

  /** A registered key signs; without one we stay in the unsigned phase-1 mode. */
  private signerFromConfig(): Signer {
    const { key_id, key_type, private_key } = this.loaded.config;
    if (!private_key) return unsignedSigner(key_id, key_type);
    try {
      return softwareSigner(key_id, key_type, private_key);
    } catch (err) {
      // A broken key must not take the app down: it still shows requests, it
      // just cannot sign — which the status bar reports.
      console.error("[approver-web] stored key unusable:", err);
      return unsignedSigner(key_id, key_type);
    }
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
    const { servers, subject, queue } = this.config;
    const nc = await connect({ servers, name: "approver-web" });
    this.nc = nc;
    this.error = null;

    this.sub = nc.subscribe(subject, { queue });
    void this.consume(this.sub);

    void nc.closed().then((err) => {
      this.nc = null;
      this.sub = null;
      this.starting = null;
      if (err) this.error = err.message;
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

  /** Sign a decision and answer into the request's reply inbox. */
  async decide(
    nonce: string,
    behavior: Behavior,
    reason: string,
    updatedInput: Record<string, unknown> | null,
  ): Promise<SignedReply> {
    const entry = this.entries.get(nonce);
    if (!entry) {
      throw new DecisionError("this request is gone (expired, or already answered)");
    }

    const reply = await buildSignedReply(entry.pending.request, {
      behavior,
      signer: this.signer,
      reason,
      updatedInput,
    });

    entry.msg.respond(encoder.encode(JSON.stringify(reply)));
    this.entries.delete(nonce);
    this.notify();
    return reply;
  }

  /**
   * Register (or rotate) this app's key with a one-time token (§6).
   *
   * Order matters and mirrors both Python responders: generate in memory →
   * publish the public half → **persist only on `ok:true`**. A rejected
   * registration must not clobber a key that still works.
   */
  async register(token: string): Promise<string> {
    const keyId = token.split(".", 1)[0];
    if (!keyId) throw new RegistrationError("token has an empty key_id");

    await this.start();
    const nc = this.nc;
    if (!nc || nc.isClosed()) {
      throw new RegistrationError(this.error ?? "not connected to NATS");
    }

    const key = generateP256();
    const request = {
      v: PROTOCOL_VERSION,
      token,
      key_id: keyId,
      pubkey: key.publicB64,
      key_type: "p256" as const,
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
    if (!reply.data.ok) throw new RegistrationError(reply.data.error ?? "registration rejected");

    this.loaded.config = {
      ...this.loaded.config,
      key_id: keyId,
      key_type: "p256",
      private_key: key.privateB64,
      public_key: key.publicB64,
    };
    saveConfig(this.loaded.config, this.loaded.path);
    this.loaded.fromFile = true;
    this.signer = this.signerFromConfig();
    this.notify();
    return keyId;
  }

  status(): ResponderStatus {
    return {
      connected: this.nc !== null && !this.nc.isClosed(),
      servers: this.config.servers,
      subject: this.config.subject,
      queue: this.config.queue,
      key_id: this.signer.keyId,
      key_type: this.signer.keyType,
      signing_mode: this.signer.mode,
      registered: this.signer.mode !== "unsigned",
      public_key: this.loaded.config.public_key ?? null,
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
