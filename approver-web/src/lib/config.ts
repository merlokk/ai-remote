/**
 * config.json loader (server only).
 *
 * Deliberately forgiving, unlike `lib/config.py`: the Python responder fails
 * fast when its config has no key because without one it cannot sign, while
 * phase 1 here has nothing to fail about. Every field has a default, so the app
 * runs against a stock local NATS with no config file at all. Phase 2 — which
 * does need a registered key — is where this starts refusing to start.
 *
 * Location: $AI_REMOTE_WEB_CONFIG, else ./config.json next to package.json.
 */
import { closeSync, fsyncSync, openSync, readFileSync, renameSync, writeFileSync } from "node:fs";
import { join, resolve } from "node:path";

import { type Config, configSchema, type RegisteredClient } from "./schemas.ts";

export interface LoadedConfig {
  config: Config;
  /** Absolute path we looked at — printed in the UI so nobody edits the wrong file. */
  path: string;
  /** False when the file was missing and pure defaults are in effect. */
  fromFile: boolean;
}

/**
 * Move a pre-`clients` config's single key into `clients`, and drop the legacy
 * fields so nothing can read a stale second copy.
 *
 * Reading a config must not write one, so this runs in memory only: the file on
 * disk stays in the old shape until the next registration saves the new one.
 * `clients` wins where both describe the same `key_id` — it is the shape this
 * version maintains.
 */
export function migrateLegacyClient(config: Config): { config: Config; moved: boolean } {
  const { key_id, key_type, public_key, public_key_raw, ...rest } = config;
  const legacy = key_id && public_key && public_key_raw;
  const clients = { ...rest.clients };
  const moved = Boolean(legacy) && !(key_id! in clients);
  if (moved) {
    clients[key_id!] = { key_type: key_type ?? "p256", public_key: public_key!, public_key_raw: public_key_raw! };
  }
  return { config: { ...rest, clients }, moved };
}

export type ClientLookup =
  | { ok: true; key_id: string; client: RegisteredClient }
  | { ok: false; error: string };

/**
 * Which registered key a decision is to be verified against.
 *
 * `keyId` is what the browser posted — it holds one key and knows its own
 * `key_id`. A decision that names none resolves only while exactly one browser
 * is registered: with two, guessing would verify against the wrong key and
 * surface as "the signature does not match", which is the one failure an
 * operator cannot act on. Naming both instead is the whole point of the message.
 */
export function selectClient(config: Config, keyId?: string | null): ClientLookup {
  const registered = Object.keys(config.clients).sort();
  if (registered.length === 0) {
    return { ok: false, error: "no key is registered — register a browser first" };
  }
  if (!keyId) {
    if (registered.length === 1) {
      return { ok: true, key_id: registered[0]!, client: config.clients[registered[0]!]! };
    }
    return {
      ok: false,
      error:
        `this decision does not say which key signed it, and ${registered.length} browsers are ` +
        `registered (${registered.join(", ")}) — reload the page`,
    };
  }
  const client = config.clients[keyId];
  if (!client) {
    return {
      ok: false,
      error:
        `nothing is registered under ${keyId} here (registered: ${registered.join(", ")}) — ` +
        `this browser's key was rotated out, or it registered against another config`,
    };
  }
  return { ok: true, key_id: keyId, client };
}

export function configPath(): string {
  const override = process.env.AI_REMOTE_WEB_CONFIG;
  // The default is statically scoped so the bundler does not decide it must
  // trace the whole project into the server output.
  if (!override) return join(process.cwd(), "config.json");
  return resolve(/* turbopackIgnore: true */ override);
}

export function loadConfig(): LoadedConfig {
  const path = configPath();
  let raw: unknown = {};
  let fromFile = false;

  try {
    // turbopackIgnore: the path is runtime configuration, not a bundled asset —
    // without this the bundler traces the whole project into the server output.
    raw = JSON.parse(readFileSync(/* turbopackIgnore: true */ path, "utf-8"));
    fromFile = true;
  } catch (err) {
    const code = (err as NodeJS.ErrnoException).code;
    if (code !== "ENOENT") {
      throw new Error(`${path} could not be read: ${(err as Error).message}`);
    }
  }

  const parsed = configSchema.safeParse(raw);
  if (!parsed.success) {
    throw new Error(`${path} is invalid: ${parsed.error.issues.map((i) => `${i.path.join(".")}: ${i.message}`).join("; ")}`);
  }
  const { config, moved } = migrateLegacyClient(parsed.data);
  if (moved) {
    console.warn(
      `[approver-web] ${path} holds a single key in the pre-clients shape; using it as ` +
        `clients["${parsed.data.key_id}"]. The new shape is written on the next registration.`,
    );
  }
  return { config, path, fromFile };
}

/**
 * Write the config atomically — temp file, fsync, rename — the way
 * `lib/config.py` does. There is no private key in here, but a half-written file
 * would still cost a registration (the token is spent) — and now it would cost
 * every *other* browser's registration too, since they share the one file.
 */
export function saveConfig(config: Config, path: string = configPath()): void {
  const tmp = `${path}.tmp`;
  const body = `${JSON.stringify(config, null, 2)}\n`;
  writeFileSync(tmp, body, { encoding: "utf-8", mode: 0o600 });
  const fd = openSync(tmp, "r+");
  try {
    fsyncSync(fd);
  } finally {
    closeSync(fd);
  }
  renameSync(tmp, path);
}
