# Claude permission approver

The applied goal of the project is to move Claude Code's permission confirmation outside the terminal. Instead of the interactive permission prompt, the `PermissionRequest` hook sends a request into NATS (request-reply), an external human responder signs the decision with an Ed25519 key, and the hook verifies the signature and hands Claude Code an `allow`/`deny` verdict. Trusted responder keys are provisioned through a separate registration process using one-time tokens. The full protocol, message contracts, and fail-safe requirements are described in sections 6–7.

A local sandbox for experimenting with [NATS](https://nats.io/) on Docker Desktop under Windows. The infrastructure comes up with a single `docker compose` (section 3): the NATS server itself with JetStream enabled, a web dashboard to observe the bus, and a `nats-box` container with the `nats` CLI for manual checks of publishes, streams, and subscriptions.

## 1. Repository rules

- **TDD.** Test first, then code. The red → green → refactor cycle: write a failing test for the behavior → the minimal code to make it green → refactor under green tests. New functionality and bugfixes come together with tests; a PR without tests for the changed behavior is not merged. The test runner is **pytest**, tests live in `tests/`, files `test_*.py`.
- **List of libraries in use** (the only approved ones):
  - **Runtime:** `nats-py` (NATS client), `cryptography` (Ed25519 / ECDSA P-256 — sign/verify).
  - **Optional runtime:** `fido2` (YubiKey / CTAP2 / WebAuthn — the ARKG `previewSign` flow of §8). **Optional on purpose:** it is needed only by `lib/yubikey.py`, so the approval flow of §6/§7 must never import it. Declared in the `yubikey` extra, not in `[project.dependencies]`.
  - **Dev/tests:** `pytest`.
  - The Python standard library — no restrictions.
  - **Where they are declared.** The single source of truth is `pyproject.toml`: runtime in `[project.dependencies]`, optional runtime in `[project.optional-dependencies].yubikey`, dev/tests in `[dependency-groups].dev` (PEP 735). Versions are locked in `uv.lock` (committed). The project is non-package (`[tool.uv] package = false`, no `[build-system]`): we do not build a distribution, there is no editable install.
  - **Installation (uv):** `uv sync` (runtime + dev), `uv sync --no-dev` (runtime only), `uv sync --extra yubikey` (adds `fido2`). No `requirements*.txt` — they do not exist.
  - **Optional deps must degrade, not crash.** A module backed by an extra has to import cleanly when the extra is absent (lazy/guarded import + an `X_AVAILABLE` flag + a `require_X()` raising an actionable error), and its tests must skip rather than fail. `lib/yubikey.py` is the reference implementation of this pattern.
- **You may not pull in new libraries without confirmation.** Any dependency outside the list above (including transitive ones that drag in noticeable weight, and dev tools) is added only after explicit sign-off from the repository owner. The preference is to solve the task with the standard library. If a new dependency really is needed — ask first, then add it and update this list.

## 2. Structure

- `nats/` — docker-compose with the NATS server, dashboard, and nats-box (CLI).
- `lib/` — reusable modules (stdlib + approved dependencies, no new ones of their own). Has an `__init__.py` (as does `approver/`) so `import lib.bus` / `from approver import protocol` resolve; the scripts additionally prepend the repo root to `sys.path` so they also work when run directly by path:
  - `lib/bus.py` — JSON request-reply over NATS (a thin async wrapper over `nats-py`). `connect()` (async context manager, yields a `Bus`, drains on exit; defaults to `nats://127.0.0.1:4222`); `Bus.request(subject, payload, timeout=)` (NATS errors → `RequestTimeout` / `NoResponders`); `Bus.reply(subject, handler, queue=)` (handler sync or async; returning `None` publishes no reply; `queue` — queue group for multiple responders, see §6); `Bus.publish(subject, payload)` (fire-and-forget); `Bus.flush()` (push buffered messages to the server — needed before draining, see `--once` below). Used by both sides of the §7 flow.
  - `lib/config.py` — versioned, atomic JSON config store (`handler-config.json` / `responder-config.json`, see §6). `Config.load(path, default=)` (deep-copies the default if the file is missing; **missing file and no `default` → `ConfigError`** — that is how `responder.py serve` fails fast on an unregistered key; a mismatched/absent `v` → `ConfigVersionError`); `Config.save()` (atomic: temp + fsync + `os.replace`, creates parent directories, stamps `v`); dict-like access (`[]`, `[]=`, `get`, `setdefault`, `in`, iteration).
  - `lib/yubikey.py` — YubiKey / ARKG helpers over the **optional** `fido2` extra (see §8). Pure, always available: `parse_firmware_version` / `FirmwareVersion` / `MIN_ARKG_FIRMWARE`, `supports_preview_sign`, `device_info_from_info` / `DeviceInfo`, `validate_ikm` / `validate_ctx`, `load_yubico_roots`, `verify_certificate_chain`, `identify_yubico_certificate`. Needs `fido2`: `get_device_info` / `get_version`, `make_credential`, `parse_seed_public_key` / `seed_public_key`, `verify_attestation_object` / `verify_yubikey_attestation`, `enumerate_devices`. Errors: `YubiKeyError` ← `Fido2NotInstalled` / `NoAuthenticator` / `ExtensionUnsupported` / `AttestationError`. Hardware smoke test: `py -m lib.yubikey [--arkg]`.
  - `lib/yubico-fido-ca.pem` — the three Yubico **FIDO** attestation roots (U2F Root CA 457200631, FIDO Root CA 450203556, Attestation Root 1) from `developers.yubico.com/PKI`; the trust anchors `verify_yubikey_attestation` pins to. Their sha256 fingerprints are asserted in tests, so replacing this file fails the suite.
  - `lib/crypto.py` — signatures over `cryptography`, two schemes selected by a `key_type` tag: `"ed25519"` (default) and `"p256"` (ECDSA P-256 / secp256r1 with SHA-256). Constants `ED25519` / `P256` / `KEY_TYPES` / `DEFAULT_KEY_TYPE`. API: `generate_keypair(key_type=ED25519)` / `KeyPair` (`.generate(key_type)`, `.from_private_b64(b64, key_type)`, `.private_b64()`, `.public_b64()`, `.sign(bytes)`, attr `.key_type`), `sign(private_b64, bytes, key_type=ED25519)`, `verify(public_b64, bytes, sig_b64, key_type=ED25519) -> bool`. Keys/signatures are standard base64 — ed25519: priv/pub 32B, sig 64B, deterministic; p256: priv 32B scalar, pub 33B compressed point, sig DER (variable length), randomized. The `key_type` is **not** part of the signed bytes: it is pinned by the trusted allowlist entry (bound to `key_id`), so the verifier always uses the registered scheme, never one chosen by the reply. The module is protocol-agnostic: it signs/verifies raw `bytes`, assembling the §7 "signing bytes" is up to the caller. `verify` is fail-safe: any malformed input / unknown `key_type` → `False`, never raises (matching the hook's fail-safe, §7).
- `approver/` — the permission-approval code (NATS Request-Reply + Ed25519/ECDSA P-256, see §6/§7):
  - `approver/protocol.py` — the shared wire-format contract (§7): `PROTOCOL_VERSION`, `canonical_json` (sort_keys, no spaces), `canonical_sha256`, `signing_bytes(...)` (fixed field order joined by `\n`, `reason` last). One implementation for both sides — the responder signs, the hook re-verifies.
  - `approver/responder.py` — the human responder. `register <token> [--key-type ed25519|p256]`: generates a new key pair of the chosen scheme (default `ed25519`), registers the public key + `key_type` over `registrations` using a one-time token, saves the pair (with `key_type`) to `responder-config.json` **only on `ok:true`** (a rejection does not clobber a working config). `serve`: subscribes to `approvals.*` (queue group `approvers`), prompts the operator on the console, signs the reply with the config's `key_type` (§7). Pure functions `parse_key_id` / `build_registration_request` / `build_reply` — tested without NATS. Run: `py approver/responder.py {register|serve}`.
  - `approver/registration_handler.py` (in §6 — `registration-handler.py`; underscore so the module is importable) — the allowlist owner. `--get-token <key_id>`: mints a one-time token `<key_id>.<secret>` (TTL 15 min), writes it to `pending_tokens`, prints the token to stdout. Without the flag — listens on `registrations`: finds the token, checks `key_id`/expiry/`key_type`, writes `clients[key_id]` (with the pinned `key_type`; rotation), consumes the token (one-time, only on success). `--once` (serve mode): exit after the first **successful** registration (`flush()` first, so the ack is on the wire before draining) — used by the e2e scripts instead of a background process that must be killed. Pure functions `validate_key_id` / `add_pending_token` / `sweep_expired` (expired tokens are dropped on every mint) / `get_token` / `handle_registration` + `make_handler` (reloads the config from disk on every message + `asyncio.Lock` around the read-modify-write). Errors: `bad request|token unknown|key_id mismatch|expired`. Run: `py approver/registration_handler.py [--get-token <key_id>] [--once] [--config <path>] [--servers <url>] [--ttl <sec>]`.
  - `approver/hook.py` — the Claude Code `PermissionRequest` hook (see §7). Reads the payload from stdin, checks `hook_event_name`, sends `nats request approvals.<session_id>` (with nonce/`input_sha256`/ts), verifies the signed reply against the allowlist (`clients` from `handler-config.json`), and prints the `decision` to stdout. Pure functions `build_request` / `verify_reply` / `decision_output` + `allowlist_from_config` / `servers_from_config` / `timeout_from_config` + `request_decision` (orchestration). **Fail-safe:** any error / invalid signature / mismatch / untrusted `key_id` → exit ≠0 and ≠2 (the normal prompt); the decision is delivered only via exit-0 JSON, never a "silent allow". The NATS server(s) and the approval timeout (default 60s) are read from `handler-config.json` (`servers` / `timeout` keys); only the config-file location is external — env `AI_REMOTE_HANDLER_CONFIG` or `--config`. To activate it, wire it up **yourself** in Claude Code's `settings.json` (the repo ships no such hook entry — `.claude/settings.local.json` only holds `permissions`): a `PermissionRequest` hook, matcher `*`, command `py <repo>\approver\hook.py` (see the README for the exact snippet).
  - **Runtime configs with secrets** (`responder-config.json` — the private key; `handler-config.json` — token secrets in `pending_tokens`) are not committed to git (see `.gitignore`). Committed alongside them are `handler-config.example.json` (a usable starter: `servers`/`timeout` set, empty `clients`/`pending_tokens`) and `responder-config.example.json` (format reference — the real file is generated by `responder.py register`).
- `scripts/e2e-registration.cmd` — a command-file e2e of registration (§6): mints a token → brings up the handler with `--once` (it exits itself after the first successful registration) → `responder register` (with retries until ready) → checks `clients[key_id].pubkey` against the responder's `public_key`. Throwaway configs in `%TEMP%`, does not touch the repository. Requires NATS on localhost and the `py` launcher. Exit 0 = PASS, 1 = FAIL. Run: `scripts\e2e-registration.cmd`.
- `scripts/e2e-approval.cmd` — a command-file e2e of the approval loop (§7): registers a responder → starts real `responder.py serve` in the background (the operator's `allow` is fed from a redirected answers file instead of typed) → pipes a `PermissionRequest` into real `hook.py` (retries until the responder is subscribed) → verifies the hook emits a signed `allow` decision. Exercises the actual processes / stdin-stdout / exit codes, not just in-process calls. Throwaway configs in `%TEMP%`, does not touch the repository. Requires NATS on localhost and the `py` launcher. Exit 0 = PASS, 1 = FAIL. Run: `scripts\e2e-approval.cmd`.
- `tests/` — pytest tests (`test_*.py`), see §1. `conftest.py`: the `requires_nats` marker (skips integration tests when NATS is unreachable) and `run_async()` (drives async bodies via `asyncio.run` — we do not add `pytest-asyncio`). `test_yubikey.py` holds the YubiKey tests that need nothing external; `test_yubikey_fido2.py` guards the rest with `pytest.importorskip("fido2")` and fakes the hardware (synthetic ARKG seed key + throwaway attestation CA), so no YubiKey is required to run the suite.
- `pyproject.toml` — project metadata and dependencies (runtime + dev group); the source of truth for dependencies. In `[tool.pytest.ini_options]`: `pythonpath=["."]` (importing `lib.*` in a non-package project), `testpaths=["tests"]`, `--basetemp=.pytest_tmp` (the default temp root is unavailable in this sandbox).
- `uv.lock` — locked versions (uv), committed to the repository.
- `.gitignore` — `__pycache__/` + `*.py[cod]`, `.venv/`, `.pytest_cache/`, `.pytest_tmp/`, `.idea/`, and the two secret-bearing runtime configs `approver/responder-config.json` / `approver/handler-config.json` (their `*.example.json` siblings carry no secrets and **are** committed).

## 3. Infrastructure (`nats/docker-compose.yml`)

Bring up: `cd nats && docker compose up -d`

| Service          | Container        | Ports (host→container)          | Purpose                                                                   |
|------------------|------------------|---------------------------------|---------------------------------------------------------------------------|
| `nats`           | `nats-server`    | 4222→4222, 8222→8222, 6222→6222 | client; HTTP monitoring (8222 — `/varz`, `/jsz`, `/connz`); clustering    |
| `nats-dashboard` | `nats-dashboard` | 8080→**80**                     | Web UI (http://localhost:8080/)                                           |
| `nats-box`       | `nats-box`       | —                               | `nats` CLI (`docker exec -it nats-box sh`)                                |

JetStream data lives on the named volume `nats_data` (mounted at `/data`, server started with `--store_dir=/data`), so streams survive `docker compose down`; `docker compose down -v` wipes them.


## 4. NATS: key concepts
- `-js` only **enables** JetStream, it does not turn on persistence globally.
- Persistence is targeted: via a **stream** that captures the given subjects (`nats stream add ORDERS --subjects "orders.*"`). Subjects without a stream behave like Core NATS (fire-and-forget).
- `nats pub` prints "Published" = confirmation of sending, NOT of delivery/storage.

## 5. Python (host)
- Run via the **`py`** launcher (Python 3.14.6): `py script.py`, `py -m pytest`, `py -c "..."`.
- The real interpreter that `py` points to: `C:\Users\User\AppData\Local\Python\pythoncore-3.14-64\python.exe`.
  `C:\...\WindowsApps\python.exe` is the Microsoft Store stub, do NOT use it.

## 6. Responder registration (bootstrapping trusted keys)

How the responder's public key gets into the allowlist that `hook.py` checks. Keys are not written by hand — the responder generates its own pair and registers the public half using a one-time token. Registration is a prerequisite step: without a trusted key the approval flow (section 7) will not work.

**Roles and configs:**
- `registration-handler.py` — the allowlist owner. Stores it in its own JSON config next to the script (`handler-config.json`); this same config is read by `hook.py` when checking `key_id`. Issues one-time tokens and listens on the `registrations` subject.
- `responder.py` — stores its `key_id` and **private** key in its own JSON config (`responder-config.json`). The private key never leaves for the bus. The `key_id` is not chosen by the responder — it comes inside the token (see below).

`handler-config.json`:
```json
{
  "v": 1,
  "servers": "nats://127.0.0.1:4222",
  "timeout": 60,
  "pending_tokens": [
    { "key_id": "approver-1", "token": "approver-1.<b64 32 bytes>", "expires_ts": 1737346500 }
  ],
  "clients": {
    "approver-1": { "pubkey": "<b64 public>", "key_type": "ed25519", "registered_ts": 1737345600 }
  }
}
```
- `clients` (a map `key_id → {pubkey, key_type, …}`) is precisely the allowlist for `hook.py`. `key_type` (`ed25519`/`p256`) is the signature scheme pinned for this key at registration; an entry without it is treated as `ed25519` (backward compatible). The hook verifies with **this** scheme, never one chosen by the reply.
- `servers` / `timeout` (both optional) configure `hook.py`'s NATS connection and how long it waits for a human decision; absent/invalid values fall back to `nats://127.0.0.1:4222` / 60s. The registration handler preserves these keys across writes.

`responder-config.json`:
```json
{
  "v": 1,
  "key_id": "approver-1",
  "key_type": "ed25519",
  "private_key": "<b64 private>",
  "public_key": "<b64 public>"
}
```
- `key_type` (`ed25519`/`p256`) records the responder's own scheme so `serve` signs with it; absent → `ed25519`.

**The token is bound to a `key_id`.** The token format is `<key_id>.<b64 32 random bytes>`: a readable `key_id` on the left, the secret on the right. A `key_id` cannot contain `.` (the first dot is the separator). The token authorizes registration of **only** that `key_id` — it cannot be used to claim or hijack someone else's slot.

**Flow:**
1. `registration-handler.py --get-token <key_id>` — generates a secret (32 bytes b64), assembles the token `<key_id>.<secret>`, puts a record `{key_id, token, expires_ts}` into `pending_tokens` (`expires_ts` defaults to now+15 min), and prints the token to stdout. The token is handed to the operator off the bus.
2. `responder.py register <token> [--key-type ed25519|p256]` — parses `key_id` from the token prefix, generates a new pair of the chosen scheme (default `ed25519`), and sends `nats request registrations` with the message (see below). The pair (`key_id` + `key_type` + private + public) is written to `responder-config.json` **only after** the handler acks `ok:true` — the order matters: a rejected registration must not clobber a working config.
3. `registration-handler` listens on `registrations`. For each message: it finds the token in `pending_tokens`, checks that it has not expired, **that the `key_id` in the message matches the `key_id` the token is bound to**, and that `key_type` (if present) is a known scheme → writes `clients[key_id] = {pubkey, key_type, registered_ts}`, **overwriting** the client key with this `key_id` (rotation) → removes the token from `pending_tokens` (one-time use) → replies with an ack in the reply-inbox. From this moment `hook.py` trusts the new key for this `key_id`.

Request (`responder.py` → `registrations`):
```json
{
  "v": 1,
  "token": "approver-1.<b64 from --get-token>",
  "key_id": "approver-1",
  "pubkey": "<b64 public>",
  "key_type": "ed25519",
  "ts": 1737346000
}
```
- `key_id` must match the prefix of `token` — the handler verifies this (mismatch → `ok:false`).
- `key_type` (`ed25519`/`p256`) is optional; absent → `ed25519`. An unknown value is rejected as `bad request`. It is stored in `clients[key_id]` and pins the scheme the hook verifies with.

Reply (`registration-handler` → reply-inbox):
```json
{ "v": 1, "ok": true, "key_id": "approver-1" }
```
- On error — `{ "v": 1, "ok": false, "error": "<token unknown|expired|key_id mismatch|bad request>" }`. A token is considered spent only on success; a spent (or otherwise unrecognized) token comes back as `token unknown`.

**Multiple clients.** `clients` may hold several `key_id`s. But `approvals.<session_id>` is a regular subject: if several responders are running in parallel, the request reaches **all** of them, and each can reply (the hook takes the first valid reply). Hence the rule: keep **one** responder running at a time, or subscribe responders to `approvals.*` via a **queue group** (then each message goes to exactly one instance). The `registrations` subject is fan-out, but the token is one-time, so duplicate registrations are safe.

**Token trust model.** A token = authorization to register, bound to its `key_id`. The holder of a valid, unexpired token can register/rotate the key of **only** that `key_id` — claiming or hijacking someone else's slot is impossible. Rotating the key of the same `key_id` (overwriting `clients[key_id]`) is intentional. Tokens are short-lived, issued off the bus, and not logged. The `key_id` is not a secret (it is visible in the token) — only the right-hand part is the secret.

```mermaid
sequenceDiagram
    autonumber
    participant O as Operator
    participant RH as registration-handler.py
    participant N as NATS
    participant R as responder.py
    participant HK as hook.py

    O->>RH: --get-token <key_id>
    RH->>RH: token = key_id.secret + expires_ts → pending_tokens
    RH-->>O: prints the token (off the bus)
    O->>R: register <token> [--key-type]
    R->>R: key_id from token prefix + new ed25519/p256 pair (in memory)
    R->>N: request registrations {token, key_id, pubkey, key_type}
    N->>RH: delivery
    alt token valid, not expired, key_id matches, key_type known
        RH->>RH: clients[key_id] = {pubkey, key_type} (overwrite) + spend the token
        RH-->>R: ack {ok:true}
        R->>R: only now: keypair → responder-config.json
    else token unknown/expired/key_id mismatch
        RH-->>R: ack {ok:false, error}
        R->>R: config left untouched
    end
    Note over RH,HK: hook.py reads clients as the allowlist when checking key_id
```

## 7. Feature: PermissionRequest hook → NATS Request-Reply (signed)

Replacing Claude Code's interactive permission prompt with external confirmation over NATS.

- **Hook event:** `PermissionRequest` (not `PreToolUse`). It fires only when Claude would actually reach the prompt; a rule-based auto-allow does not go onto the bus.
- **Matcher:** `*` (all tools).
- **Flow:** `hook.py` (on the host) reads stdin JSON, **checks `hook_event_name == "PermissionRequest"`** (otherwise the payload is not ours → fall through to the normal prompt) → sends `nats request approvals.<session_id>` with a nonce → `responder.py` (manual human input) signs the decision (Ed25519 or ECDSA P-256, per its configured `key_type`) and replies to the request's reply-inbox → the hook verifies the signature using the scheme pinned in the allowlist for that `key_id` → prints `hookSpecificOutput.decision.behavior` (`allow`/`deny`) to stdout. There is no separate `.decision` subject: the reply travels over the request-reply channel.
- **We sign the entire content of the reply** — the concatenation `v + session_id + nonce + tool_name + input_sha256 + behavior + updated_input_sha256 + ts + reason` (nonce = anti-replay; `input_sha256` = the hash of `tool_input`, so the decision cannot be replayed onto a different command; `updated_input_sha256` is under the signature, otherwise what actually gets executed could be swapped; `reason`/`v`/`ts` are under the signature too). The exact format is in the "Signing bytes" section below.
- **Fail-safe:** any error (NATS unavailable, timeout, a bad/absent signature, a foreign nonce/key_id) → exit ≠ 0 and ≠ 2 → fall through to the **normal prompt**. Never a "silent allow". (Exit 2 is a blocking error: stdout is ignored, stderr goes to Claude; for `PermissionRequest` this is probably deny, but the exact semantics are not pinned down in the docs — so do NOT use it on errors, the decision is delivered only via exit-0 JSON.)
- **Client:** `nats-py`; **cryptography:** `cryptography` (Ed25519 or ECDSA P-256, chosen per key via `key_type`).

### 7.1 Sequence diagram

```mermaid
sequenceDiagram
    autonumber
    participant CC as Claude Code
    participant H as hook.py (host)
    participant N as NATS
    participant R as responder.py (operator)

    CC->>H: stdin JSON (PermissionRequest)
    Note over H: check hook_event_name<br/>otherwise → normal prompt (exit ≠ 0,2)
    H->>H: nonce, input_sha256, ts
    H->>N: request approvals.session_id
    N->>R: request delivery
    Note over R: a human decides allow/deny
    R->>R: signature over signing bytes (ed25519/p256 per key_type)
    R-->>N: reply (behavior, sig, key_id, ...)
    N-->>H: reply to the inbox

    alt echo fields + key_id + updated_input_sha256 + sig valid
        alt behavior == allow
            H-->>CC: stdout allow (+updatedInput), exit 0
        else behavior == deny
            H-->>CC: stdout deny, exit 0
        end
    else any check failed / timeout / error
        H-->>CC: exit ≠ 0 and ≠ 2 → normal prompt
    end
```

`PermissionRequest` hook contract:
- stdin: `{ hook_event_name, session_id, prompt_id, transcript_path, tool_name, tool_input, permission_mode, cwd }` — the hook must check `hook_event_name == "PermissionRequest"`. (`prompt_id` was added in Claude Code v2.1.196+; the hook does not need it, but it arrives in the payload.)
- stdout (exit 0): `{"hookSpecificOutput":{"hookEventName":"PermissionRequest","decision":{"behavior":"allow"|"deny","updatedInput"?:{…}}}}`
- exit codes: `0` — stdout is parsed as JSON (JSON is processed only on exit 0); `2` — a blocking error (stdout is ignored, stderr goes to Claude; the effect depends on the event, for `PermissionRequest` — probably deny, but the exact semantics are not pinned down in the docs); anything else (including `1`) — a non-blocking error, the normal prompt. The allow/deny decision is always delivered via exit-0 JSON, not via exit 2.

NATS message contract:

Request subject: `approvals.<session_id>`. The reply goes to the reply-inbox (request-reply).

Request (`hook.py` → the bus):
```json
{
  "v": 1,
  "session_id": "abc123",
  "tool_name": "Bash",
  "tool_input": { "command": "rm -rf build" },
  "input_sha256": "<hex sha256 of canonical JSON of tool_input: sort_keys=True, separators=(',',':')>",
  "permission_mode": "default",
  "cwd": "E:\\projects\\ai-remote\\nats",
  "nonce": "<b64, 32 random bytes>",
  "ts": 1737345600
}
```

Reply (`responder.py` → reply-inbox). The fields `v/session_id/tool_name/input_sha256/nonce/ts` are echoed from the request; `behavior/reason/updated_input` come from the responder:
```json
{
  "v": 1,
  "behavior": "allow",
  "reason": "approved by operator",
  "session_id": "abc123",
  "tool_name": "Bash",
  "input_sha256": "<echo from request>",
  "nonce": "<echo from request>",
  "ts": 1737345600,
  "updated_input": { "command": "npm ci" },
  "key_id": "approver-1",
  "sig": "<b64 signature over the signing bytes; scheme per the key's key_type>"
}
```
- `updated_input` (optional, object) — overrides the tool arguments; if set, the hook prints it in `decision.updatedInput`. It enters the signature as `updated_input_sha256` (see below).
- The field is optional and applied only when `behavior == "allow"`. If it is absent — the signing bytes use an empty string `""` in the `updated_input_sha256` position.

**Signing bytes** (signed by the responder, verified by the hook) — a raw concatenation of the fields in a **fixed order** with the `\n` separator, utf-8:
```
str(v) + "\n" + session_id + "\n" + nonce + "\n" + tool_name + "\n" + input_sha256 + "\n" + behavior + "\n" + updated_input_sha256 + "\n" + str(ts) + "\n" + reason
```
- `updated_input_sha256` = hex sha256 of the canonical JSON of `updated_input`, or `""` if the field is absent. The hook recomputes the hash from the received `updated_input` and compares — just like with `input_sha256`. The JSON canonicalization for both hashes is the same: `json.dumps(..., sort_keys=True, separators=(',',':'))`, utf-8 → sha256.
- `reason` comes **last** on purpose: it is the only free-text field and may contain `\n`; being the tail of the string, it stays unambiguous. None of the other fields contain a newline.

The order and separator must not be changed — both sides assemble the string identically.

Checks in `hook.py` before trusting a reply (any that fails → fall through to the normal prompt):
- `v` matches the expected protocol version;
- `nonce`, `session_id`, `tool_name`, `input_sha256`, `ts` match what was sent (anti-replay + binding to the command);
- `key_id` is present in the allowlist of trusted public keys (the allowlist is populated through registration — see section 6 "Responder registration" above; `key_id` itself is not part of the signing bytes — it is bound to the signature indirectly: it selects the public key **and the `key_type`**, and a signature made by a different key or a different scheme will not pass verification). The allowlist entry's `key_type` (default `ed25519`) selects the verification scheme; an unknown `key_type` is a fail-safe reject. `key_type` is taken from the trusted allowlist, never from the reply — so a reply cannot downgrade or swap the algorithm;
- `reason` is a string (it defaults to `""` when absent). A non-string `reason` cannot be assembled into the signing bytes, so it is rejected before the signature check rather than raising;
- if `updated_input` is present — the hook recomputes `sha256(canonical(updated_input))` and substitutes it into the signing bytes in the `updated_input_sha256` position (this hash is **not** transmitted as a separate field in the reply, unlike `input_sha256`, which is echoed back in the reply). Consistency is guaranteed by the `sig` check itself: if the responder signed a different `updated_input_sha256`, the signature will not match. If the field is absent — an empty string (`""`) takes its place in the signing bytes;
- `sig` is valid for the signing bytes under the corresponding public key (after this `behavior`/`reason`/`updated_input` can be trusted);
- `behavior ∈ {allow, deny}`; `updated_input` is honored only on `allow`.

**Privacy:** `tool_input` goes onto the bus as-is — for Bash that is the full command, for Write the file contents. The `approvals.<session_id>` subject and access to NATS must be restricted; do not connect untrusted subscribers.

## 8. YubiKey / ARKG (`lib/yubikey.py`, optional `fido2` extra)

A standalone library around a YubiKey's **ARKG** (Asynchronous Remote Key Generation) support, exposed through the CTAP2 `previewSign` extension. It is **not** wired into the §6/§7 approval flow — it is a separate capability that flow may later build on.

**Why ARKG.** The authenticator holds one *seed* key pair. Whoever has the seed **public** key can derive unlimited fresh public keys **offline** — mutually unlinkable, and unlinkable to the seed — by choosing a random `ikm` plus a purpose label `ctx`. Only the authenticator can produce the matching private key, and only when handed the `key_handle` that came out of the derivation. Hence step 2 below needs no hardware and no touch.

**Install:** `uv sync --extra yubikey`. **Hardware:** a YubiKey on firmware **5.8.0+** advertising `previewSign`. **On Windows: run from an administrator terminal** — otherwise `fido2` uses the native Windows WebAuthn API, which silently drops the `previewSign` output; `make_credential` raises `ExtensionUnsupported` instead of returning a half-built result.

### 8.1 The four entry points

| # | Call | Hardware? | Returns |
|---|------|-----------|---------|
| 0 | `get_version()` / `get_device_info()` | plugged in, no touch | `FirmwareVersion` / `DeviceInfo` (firmware, aaguid, extensions, `supports_preview_sign`, `meets_arkg_firmware`) |
| 1 | `make_credential(...)` | touch (maybe PIN) | `MakeCredentialResult` — `key_handle`, `seed_public_key_cbor`, `credential_id`, `aaguid`, `client_data_hash`, `attestation_object`, `seed_attestation_object` |
| 2 | `seed_public_key(result, ctx=..., ikm=None)` | **no** | `DerivedKey` — `derived_public_key` (+`_cbor`), `arkg_args` (+`_cbor`), `ikm`, `ctx`, `key_handle` |
| 3 | `verify_yubikey_attestation(result)` | **no** | `AttestationCheck` — `is_yubikey` plus `reasons` explaining a False |

- **Versions.** CTAP2 reports `firmwareVersion` packed as `major<<16 | minor<<8 | patch` (5.8.0 = `0x050800`). `parse_firmware_version` decodes it; `raw == 0` means the device did not report one → `is_known is False`. `FirmwareVersion` orders by major/minor/patch (`raw` is excluded from comparison), so `v >= MIN_ARKG_FIRMWARE` is the ARKG floor check.
- **`make_credential`** asks for `algorithms: [ESP256_SPLIT_ARKG_PLACEHOLDER]` under `previewSign.generateKey`, and defaults to `attestation="direct"` so step 3 has a certificate chain to check.
- **`seed_public_key`** is the offline half: it parses the seed key (must be an ARKG-P256 key, else `ExtensionUnsupported`) and returns the derived public key together with the COSE_Sign_Args (`{3: ESP256_SPLIT_ARKG_PLACEHOLDER, -1: key_handle_blob, -2: ctx}`) that the authenticator needs later. `ikm` defaults to 32 fresh random bytes and **must** be ≥ 32 bytes (`MIN_IKM_BYTES`) — the ARKG draft's 256-bit entropy recommendation is enforced, not suggested, because a predictable `ikm` destroys unlinkability. Same `(ikm, ctx)` reproduces the same key; either differing gives a different one.
- **To sign later** (not wrapped by this module — see the Yubico example): `get_assertion` with `previewSign.signByCredential[<cred>] = {keyHandle, tbs: sha256(msg), additionalArgs: cbor(arkg_args)}`, then verify the returned signature with `derived_public_key`.

### 8.2 "Is it a YubiKey?"

`verify_yubikey_attestation` (thin wrapper over `verify_attestation_object`) requires **three** independent gates, all of which must pass for `is_yubikey`:

1. the attestation statement verifies over `authData || client_data_hash` — the device really holds the attestation private key;
2. the certificate chain pins to a Yubico root from `lib/yubico-fido-ca.pem` (`verify_certificate_chain`; override with `roots=`, or `require_root=False` for a dev key);
3. the attestation certificate names Yubico (`identify_yubico_certificate`).

By default it checks the **credential's** attestation object, which is defined to be signed over `authData || clientDataHash` — the reliable path. `use_seed_attestation=True` instead checks the separate attestation `previewSign` returns for the generated seed key; that one is passed the same `client_data_hash`, which is the expected binding but has **not** been confirmed against hardware here (the extension is a draft). Prefer the default until it is verified on a real 5.8.0+ key.

- `chain` verification is deliberately **not** full RFC 5280 path validation (no revocation, no name constraints): it verifies each cert is directly issued by the next and that the top chains to a pinned root, which is what device attestation needs.
- **AAGUIDs are not hardcoded.** `allowed_aaguids=` narrows to specific models if a caller wants that, but there is no baked-in list: Yubico ships new AAGUIDs with new hardware and treats the FIDO MDS as authoritative, so a bundled list would reject *future* YubiKeys. The chain check is the durable identity signal; the AAGUID is reported for information.
- A name check alone proves nothing — gate 3 exists for a readable `reasons` entry and to reject an obviously foreign authenticator early. Never use it without gate 2.

### 8.3 Reference example vs. this module

Yubico's [`example_arkg.py`](https://github.com/YubicoLabs/build-with-us/blob/main/quickstart/python/example_arkg.py) was the specification for this module, but it targets an **older `previewSign` snapshot**: there the generated key arrived as a dict with a websafe-base64 `"publicKey"`. In `fido2` 2.2.1 the output is a `_SignGeneratedKey` dataclass with raw `bytes` (`key_handle`, `public_key`) plus an already-parsed `attestation_object` — so `websafe_decode` must **not** be applied, and the seed key is `CoseKey.parse(cbor.decode(generated.public_key))`. This module speaks the 2.2.1 shape; `previewSign` is marked DRAFT/experimental upstream, so pin `fido2` and re-check this on upgrade.

### 8.4 Testing without hardware

`tests/test_yubikey_fido2.py` needs no YubiKey: the ARKG seed key is assembled synthetically from two P-256 pairs in exactly the COSE shape the authenticator returns (real derivation, real curve math), and attestation runs against a throwaway root→EE CA generated in the test. What *cannot* be covered without hardware: `make_credential` itself and a full sign/verify round-trip (`fido2` exposes only ARKG `derive_public_key` — the private half lives in the authenticator). Use `py -m lib.yubikey --arkg` for that against a real key.
