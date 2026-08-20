/**
 * The registered-clients half of `config.ts` — one key_id per browser (§6).
 *
 * `clients` is what makes several browsers possible: one registered key per
 * `key_id`, so a second browser registering with its own token adds an entry
 * instead of rotating the first one out. These tests are about the two pure
 * functions that shape it — the legacy migration and the lookup a decision
 * does — plus one round trip through the file, because a config that cannot be
 * saved and re-read is a config that loses a registration.
 *
 * Run: npm test
 */
import assert from "node:assert/strict";
import { mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import test from "node:test";

import { loadConfig, migrateLegacyClient, saveConfig, selectClient } from "./config.ts";
import { configSchema } from "./schemas.ts";

/** A second pair of public encodings, so "the other browser" is a real other key. */
const A = { public_key: "AoneA".padEnd(45, "A"), public_key_raw: "BAaaa".padEnd(89, "A") };
const B = { public_key: "AtwoB".padEnd(45, "B"), public_key_raw: "BAbbb".padEnd(89, "B") };

function config(raw: Record<string, unknown>) {
  return configSchema.parse(raw);
}

function withConfigFile<T>(body: (path: string) => T): T {
  const dir = mkdtempSync(join(tmpdir(), "approver-web-config-"));
  try {
    return body(join(dir, "config.json"));
  } finally {
    rmSync(dir, { recursive: true, force: true });
  }
}

// --- the shape ------------------------------------------------------------------
test("a config with no clients parses, and is simply not registered", () => {
  const parsed = config({});
  assert.deepEqual(parsed.clients, {});
  const none = selectClient(parsed, null);
  assert.equal(none.ok, false);
  assert.equal(none.ok === false && none.error, "no key is registered — register a browser first");
});

test("two browsers coexist, each under its own key_id", () => {
  const parsed = config({
    clients: {
      "approver-web": { key_type: "p256", ...A },
      "approver-web-phone": { key_type: "p256", ...B },
    },
  });
  assert.deepEqual(Object.keys(parsed.clients).sort(), ["approver-web", "approver-web-phone"]);

  const first = selectClient(parsed, "approver-web");
  assert.equal(first.ok && first.client.public_key, A.public_key);
  const second = selectClient(parsed, "approver-web-phone");
  assert.equal(second.ok && second.client.public_key, B.public_key);
});

// --- the lookup one decision does ----------------------------------------------
test("a decision that names no key_id resolves while exactly one is registered", () => {
  const parsed = config({ clients: { "approver-web": { key_type: "p256", ...A } } });
  const only = selectClient(parsed, null);
  assert.equal(only.ok && only.key_id, "approver-web");
});

test("a decision that names no key_id is refused once two browsers are registered", () => {
  // Guessing would verify against the wrong key and read as a bad signature.
  const parsed = config({
    clients: {
      "approver-web": { key_type: "p256", ...A },
      "approver-web-phone": { key_type: "p256", ...B },
    },
  });
  const ambiguous = selectClient(parsed, null);
  assert.equal(ambiguous.ok, false);
  const message = ambiguous.ok === false ? ambiguous.error : "";
  assert.match(message, /which key/);
  // Naming them is the point: this is the message an operator has to act on.
  assert.match(message, /approver-web, approver-web-phone/);
});

test("a key_id nothing is registered under is refused, and says so", () => {
  const parsed = config({ clients: { "approver-web": { key_type: "p256", ...A } } });
  const gone = selectClient(parsed, "approver-web-phone");
  assert.equal(gone.ok, false);
  assert.match(gone.ok === false ? gone.error : "", /approver-web-phone/);
});

// --- the legacy single-key shape ------------------------------------------------
test("a pre-clients config migrates its one key into clients", () => {
  const { config: migrated, moved } = migrateLegacyClient(
    config({ key_id: "approver-web", key_type: "p256", ...A }),
  );
  assert.equal(moved, true);
  assert.deepEqual(Object.keys(migrated.clients), ["approver-web"]);
  assert.equal(migrated.clients["approver-web"]!.public_key_raw, A.public_key_raw);
  // The legacy fields are gone, so nothing can read a stale second copy.
  assert.equal(migrated.key_id, undefined);
  assert.equal(migrated.public_key, undefined);
  assert.equal(migrated.public_key_raw, undefined);
});

test("a legacy config with no key registered migrates nothing", () => {
  const { config: migrated, moved } = migrateLegacyClient(config({ key_id: "approver-web" }));
  assert.equal(moved, false);
  assert.deepEqual(migrated.clients, {});
});

test("clients wins over the legacy fields for the same key_id", () => {
  // Both present means a config written by this version and edited by hand
  // afterwards; `clients` is the shape that is maintained.
  const { config: migrated } = migrateLegacyClient(
    config({ key_id: "approver-web", ...A, clients: { "approver-web": { key_type: "p256", ...B } } }),
  );
  assert.equal(migrated.clients["approver-web"]!.public_key, B.public_key);
});

// --- and the file ---------------------------------------------------------------
test("loadConfig migrates a legacy file in memory without rewriting it", () => {
  withConfigFile((path) => {
    const legacy = { v: 1, key_id: "approver-web", key_type: "p256", ...A };
    writeFileSync(path, JSON.stringify(legacy));
    process.env.AI_REMOTE_WEB_CONFIG = path;
    try {
      const loaded = loadConfig();
      assert.deepEqual(Object.keys(loaded.config.clients), ["approver-web"]);
      // Reading must not write: the file on disk is still the legacy one.
      const onDisk = JSON.parse(readFileSync(path, "utf-8"));
      assert.equal(onDisk.clients, undefined);
      assert.equal(onDisk.key_id, "approver-web");
    } finally {
      delete process.env.AI_REMOTE_WEB_CONFIG;
    }
  });
});

test("saveConfig then loadConfig keeps both registrations", () => {
  withConfigFile((path) => {
    const saved = config({
      clients: {
        "approver-web": { key_type: "p256", ...A, registered_ts: 1737345600 },
        "approver-web-phone": { key_type: "p256", ...B },
      },
    });
    saveConfig(saved, path);
    process.env.AI_REMOTE_WEB_CONFIG = path;
    try {
      const loaded = loadConfig();
      assert.equal(loaded.fromFile, true);
      assert.deepEqual(Object.keys(loaded.config.clients).sort(), [
        "approver-web",
        "approver-web-phone",
      ]);
      assert.equal(loaded.config.clients["approver-web"]!.registered_ts, 1737345600);
    } finally {
      delete process.env.AI_REMOTE_WEB_CONFIG;
    }
  });
});
