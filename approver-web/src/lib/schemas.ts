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
  /** Registered allowlist identity. Meaningless until phase 2 signs with it. */
  key_id: z.string().min(1).default("approver-web"),
  key_type: z.enum(KEY_TYPES).default("p256"),
  /**
   * How long a request stays on screen, seconds. Should be >= the hook's own
   * `timeout` in handler-config.json, otherwise the card vanishes while Claude
   * Code is still waiting.
   */
  request_ttl: z.number().positive().default(120),
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

/** POST /api/decision. `updated_input` is honored only on allow (§7). */
export const decisionRequestSchema = z.object({
  nonce: z.string().min(1),
  behavior: z.enum(BEHAVIORS),
  reason: z.string().max(500).default(""),
  updated_input: z.record(z.string(), z.unknown()).nullish(),
});

export type DecisionRequest = z.infer<typeof decisionRequestSchema>;
