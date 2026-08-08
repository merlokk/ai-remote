/**
 * Zod schemas — one place where every untrusted boundary is described.
 *
 * Three boundaries, three schemas:
 *   - `configSchema`        config.json on disk (server)
 *   - `permissionRequestSchema`  whatever arrives on `approvals.*` (the bus is
 *     an open subject: a stray `nats pub` must be dropped, not crash the app —
 *     same rule `lib/bus.py` follows)
 *   - `decisionFormSchema` / `decisionRequestSchema`  the browser form and the
 *     POST it turns into
 */
import { z } from "zod";

import { PROTOCOL_VERSION } from "./protocol";

export const KEY_TYPES = ["ed25519", "p256"] as const;
export const BEHAVIORS = ["allow", "deny"] as const;

export type KeyType = (typeof KEY_TYPES)[number];
export type Behavior = (typeof BEHAVIORS)[number];

// --- config.json ---------------------------------------------------------------
export const configSchema = z.object({
  v: z.number().int().default(PROTOCOL_VERSION),
  servers: z.string().min(1).default("nats://127.0.0.1:4222"),
  subject: z.string().min(1).default("approvals.*"),
  queue: z.string().min(1).default("approvers"),
  /** Registered allowlist identity — set by `register`, from the token prefix. */
  key_id: z.string().min(1).default("approver-web"),
  key_type: z.enum(KEY_TYPES).default("p256"),
  /**
   * The registered **public** key, written only after the handler acks
   * `ok:true`. There is deliberately no private key here: custody lives in the
   * browser (`browser-key.ts`). `public_key` is the base64 33-byte compressed
   * point the allowlist holds; `public_key_raw` is the 65-byte uncompressed
   * form, kept only so the server can verify what the browser signed without
   * doing a modular square root.
   */
  public_key: z.string().min(1).optional(),
  public_key_raw: z.string().min(1).optional(),
  /**
   * The registration handler's own public key (base64 Ed25519), pinned when
   * this app registered (§6). Every later registration must be answered by
   * exactly this key — a valid signature by a *different* one is a takeover of
   * the `registrations` subject, not a handler that moved.
   */
  server_key: z.string().min(1).optional(),
  /**
   * How long a request stays on screen, seconds. Should be >= the hook's own
   * `timeout` in handler-config.json, otherwise the card vanishes while Claude
   * Code is still waiting.
   */
  request_ttl: z.number().positive().default(120),
  /**
   * Where the Rust status line publishes the model and the rate limits
   * (`statusline/CLAUDE.md` §9.7). Read-only for this app, and unrelated to the
   * approval flow: nothing here answers or depends on it, it only feeds the
   * plaque. Must match `subject` in `statusline-config.json` (§9.9).
   */
  status_subject: z.string().min(1).default("status"),
});

export type Config = z.infer<typeof configSchema>;

// --- the bus -------------------------------------------------------------------
/**
 * A §7 approval request. Unknown keys are stripped: the reply echoes only the
 * fields below, so anything else is display noise at most.
 */
export const permissionRequestSchema = z.object({
  v: z.literal(PROTOCOL_VERSION),
  session_id: z.string(),
  tool_name: z.string(),
  tool_input: z.unknown().optional(),
  input_sha256: z.string(),
  permission_mode: z.string().nullish(),
  cwd: z.string().nullish(),
  nonce: z.string(),
  ts: z.number(),
});

export type PermissionRequest = z.infer<typeof permissionRequestSchema>;

// --- the browser ---------------------------------------------------------------
/**
 * The decision form. `updatedInput` is free text so the operator can see and fix
 * their own JSON; it is parsed here rather than by a JSON input widget.
 */
export const decisionFormSchema = z
  .object({
    reason: z.string().max(500, "keep it under 500 characters"),
    updatedInput: z.string(),
  })
  .superRefine((values, ctx) => {
    const text = values.updatedInput.trim();
    if (!text) return;
    let parsed: unknown;
    try {
      parsed = JSON.parse(text);
    } catch {
      ctx.addIssue({ code: "custom", path: ["updatedInput"], message: "not valid JSON" });
      return;
    }
    if (parsed === null || typeof parsed !== "object" || Array.isArray(parsed)) {
      ctx.addIssue({
        code: "custom",
        path: ["updatedInput"],
        message: "must be a JSON object, e.g. {\"command\": \"npm ci\"}",
      });
    }
  });

export type DecisionFormValues = z.infer<typeof decisionFormSchema>;

// --- registration (§6) ----------------------------------------------------------
/**
 * A one-time token is `<key_id>.<secret>`; the first dot splits it, so a
 * `key_id` cannot contain one. Same rule as `responder.parse_key_id`, checked
 * here so the browser says "that is not a token" instead of the bus doing it.
 */
export const tokenSchema = z
  .string()
  .trim()
  .min(3, "paste the token from --get-token")
  .regex(/^[^.\s]+\.[^\s]+$/, "expected '<key_id>.<secret>' — one dot, no spaces");

export const registerFormSchema = z.object({ token: tokenSchema });
export type RegisterFormValues = z.infer<typeof registerFormSchema>;

/**
 * POST /api/register. The browser generates the key and sends only public
 * material — both encodings, because the wire wants the compressed point and
 * local verification wants the uncompressed one.
 */
export const registerRequestSchema = z.object({
  token: tokenSchema,
  public_key: z.string().min(1),
  public_key_raw: z.string().min(1),
});

/**
 * What `registration_handler.py` sends back on `registrations`.
 *
 * `server_key`/`sig`/`nonce`/`ts` are the handler's signature over the whole
 * answer (§6). They are optional *here* so that a missing one produces our own
 * "this is not the handler" message rather than a zod parse error — the
 * verification in `responder.ts` is what actually insists on them.
 */
export const registrationReplySchema = z.object({
  v: z.number().optional(),
  ok: z.boolean(),
  key_id: z.string().optional(),
  error: z.string().optional(),
  nonce: z.string().optional(),
  ts: z.number().optional(),
  server_key: z.string().optional(),
  sig: z.string().optional(),
});

export type RegistrationReply = z.infer<typeof registrationReplySchema>;

/**
 * POST /api/decision. `updated_input` is honored only on allow (§7).
 *
 * `sig` comes from the browser — the server has no key to make one with. It
 * re-derives the signing bytes from the pending request plus these fields and
 * checks the signature before answering, so a client cannot get an arbitrary
 * reply onto the bus by posting a mismatched decision.
 */
export const decisionRequestSchema = z.object({
  nonce: z.string().min(1),
  behavior: z.enum(BEHAVIORS),
  reason: z.string().max(500).default(""),
  updated_input: z.record(z.string(), z.unknown()).nullish(),
  sig: z.string().min(1),
});

export type DecisionRequest = z.infer<typeof decisionRequestSchema>;
