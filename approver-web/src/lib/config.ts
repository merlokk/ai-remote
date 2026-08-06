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
import { readFileSync } from "node:fs";
import { join, resolve } from "node:path";

import { type Config, configSchema } from "./schemas";

export interface LoadedConfig {
  config: Config;
  /** Absolute path we looked at — printed in the UI so nobody edits the wrong file. */
  path: string;
  /** False when the file was missing and pure defaults are in effect. */
  fromFile: boolean;
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
  return { config: parsed.data, path, fromFile };
}
