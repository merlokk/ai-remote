# ai-remote — Claude Code permission approver over NATS

Move Claude Code's permission prompt **out of the terminal**. Instead of the
interactive "allow / deny?" prompt, a `PermissionRequest` hook publishes the
request onto [NATS](https://nats.io/), a human responder somewhere else signs
the decision with an **Ed25519** or **ECDSA P-256** key, and the hook verifies
that signature before handing Claude Code an `allow` / `deny` verdict.

> Full protocol, message contracts and design rationale live in
> [`CLAUDE.md`](CLAUDE.md) §6–§7. This README is the practical "what it is / how
> to run it / how to check it works" guide.

## Why

The built-in permission prompt assumes the person driving Claude Code is sitting
at the terminal. This project lets a **remote human** approve (or deny)
individual tool calls:

- **Approve from elsewhere.** The decision is made by whoever is subscribed to
  the bus, not by whoever launched the session.
- **Signed & tamper-evident.** Every decision is signed (Ed25519 or ECDSA P-256,
  per the key's `key_type`) over the exact command (`tool_input` hash), a
  per-request nonce (anti-replay) and the behavior. A reply that doesn't verify
  against a *registered* key — using the scheme pinned for it — is rejected.
- **Fail-safe by design.** NATS down, a timeout, a bad/absent signature, an
  untrusted key — **any** failure falls back to the normal interactive prompt.
  There is never a "silent allow".
- **Trust is bootstrapped, not hand-edited.** Responder keys enter the allowlist
  through a one-time-token registration flow, so no public key is ever pasted in
  by hand.

## How it fits together

```
Claude Code ──stdin──▶ hook.py ──approvals.<sid>──▶ NATS ──▶ responder.py (human: allow/deny + Ed25519 sign)
     ▲                    │                                         │
     └──── allow/deny ────┘◀──────────── signed reply ──────────────┘
                          (hook verifies sig against the allowlist)

registration_handler.py  ──▶ owns handler-config.json (the allowlist `clients` + one-time `pending_tokens`)
```

| Component | Role |
|-----------|------|
| `approver/hook.py` | Claude Code `PermissionRequest` hook. Sends the request, verifies the signed reply against the allowlist, prints the decision. Fail-safe. |
| `approver/responder.py` | The human side. `register` bootstraps a key; `serve` prompts the operator and signs each decision. |
| `approver/registration_handler.py` | Owns the allowlist (`handler-config.json`). Mints one-time tokens and registers responder public keys. |
| `approver/protocol.py` | Shared wire-format: canonical JSON, hashes, and the exact "signing bytes" both sides assemble identically. |
| `lib/bus.py` | Thin async JSON request-reply wrapper over `nats-py`. |
| `lib/config.py` | Versioned, atomic JSON config store. |
| `lib/crypto.py` | Ed25519 / ECDSA P-256 keygen / sign / verify (fail-safe verify; scheme picked by `key_type`). |
| `lib/yubikey.py` | **Optional, standalone:** YubiKey ARKG (`previewSign`) — version, `makeCredential`, offline key derivation, "is it a YubiKey?" attestation. Needs the `yubikey` extra. |
| `nats/` | `docker compose` sandbox: NATS + JetStream, a web dashboard, and `nats-box` (the `nats` CLI). |
| `scripts/` | Windows command-file end-to-end checks: `e2e-registration.cmd` (§6 flow) and `e2e-approval.cmd` (§7 flow). |

## Prerequisites

- **Docker Desktop** (for the NATS sandbox).
- **Python 3.14** via the **`py`** launcher (see `CLAUDE.md` §5).
- **[uv](https://docs.astral.sh/uv/)** for dependency management.

## Setup

```bash
# 1. Install dependencies (runtime + dev) into a local .venv
uv sync

# 2. Bring up the NATS sandbox (server + dashboard + nats-box)
cd nats && docker compose up -d && cd ..

# Optional: the YubiKey/ARKG library (lib/yubikey.py) needs the `fido2` extra
uv sync --extra yubikey
```

The `yubikey` extra is **optional and separate** from the approval flow — nothing in
`approver/` imports `fido2`. Without it, `lib/yubikey.py` still imports and its pure
helpers work; its device features raise a clear error and its tests skip.

Sample configs are committed as `approver/*.example.json` (the real files hold
secrets and are git-ignored). To customize the NATS server or approval timeout,
copy the handler starter and edit it — otherwise the defaults are used and the
file is created automatically on first `--get-token`:

```bash
cp approver/handler-config.example.json approver/handler-config.json
```

`responder-config.example.json` is a format reference only — the real
`responder-config.json` is generated for you by `responder.py register`.

Once up:

- NATS client port: `nats://127.0.0.1:4222`
- HTTP monitoring: <http://localhost:8222/varz>
- Web dashboard: <http://localhost:8080/>
- `nats` CLI inside the box: `docker exec -it nats-box sh`


## Verify it works

### 1. Run the unit tests

Pure-logic tests need nothing external; the bus tests auto-skip when NATS is
unreachable.

```bash
py -m pytest -q
```

Expected: all tests pass (bus/integration tests run only if NATS is up on
`127.0.0.1:4222`).

### 2. Run the end-to-end checks (Windows)

Both scripts run against a **live NATS**, using throwaway configs in `%TEMP%`
(the repo is left untouched). Exit `0` = PASS, `1` = FAIL.

```bat
REM registration: token -> register -> allowlist matches the responder key
scripts\e2e-registration.cmd

REM approval loop: responder serve (auto-allow) -> hook.py -> verified `allow`
scripts\e2e-approval.cmd
```

`e2e-approval.cmd` runs the real `responder.py` and `hook.py` as separate
processes and checks the whole signed loop end to end (the operator's `allow` is
fed from a file instead of typed). It's the automated version of the manual
walkthrough below.

### 3. Smoke-test the full approval loop by hand

This walks the whole flow **without Claude Code** — you play Claude by piping a
fake `PermissionRequest` into the hook. Use three terminals.

> The commands below are written for **bash/Git Bash or PowerShell** (both keep
> single-quoted JSON intact). `cmd.exe` mangles the quoting in step 3 — use one of
> the `scripts\*.cmd` e2e runs above instead, or a here-doc/file for the payload.

**Step 1 — bootstrap a responder key.** In **Terminal A**, serve the
registration handler (`--once` makes it exit after the first success):

```bash
py approver/registration_handler.py --once
```

In **Terminal B**, mint a token and register. The handler reloads its config per
message, so it picks up the token even though it's already serving:

```bash
# mint a one-time token for key_id "approver-1" (token is printed to stdout)
py approver/registration_handler.py --get-token approver-1
#   -> approver-1.<secret>

# register: generates a fresh key pair (Ed25519 by default) and sends the public
# half to the handler, which — on success — writes it into its own
# approver/handler-config.json -> clients["approver-1"]. The private key is saved
# to approver/responder-config.json only after that ack, so a rejected
# registration never clobbers a working config.
py approver/responder.py register "approver-1.<secret>"

# want ECDSA P-256 instead of the default Ed25519? add --key-type p256
py approver/responder.py register "approver-1.<secret>" --key-type p256
```

Terminal A exits (`--once`) once registration succeeds.

**Step 2 — run the responder.** In **Terminal B**, become the human approver:

```bash
py approver/responder.py serve
```

**Step 3 — pretend to be Claude Code.** In **Terminal C**, ask for a decision:

```bash
echo '{"hook_event_name":"PermissionRequest","session_id":"smoke","tool_name":"Bash","tool_input":{"command":"echo hello"},"permission_mode":"default","cwd":"."}' | py approver/hook.py
```

In **Terminal B** you'll see the request; answer `a` (allow) or `d` (deny).
**Terminal C** then prints the signed, verified decision and exits `0`:

```json
{"hookSpecificOutput":{"hookEventName":"PermissionRequest","decision":{"behavior":"allow"}}}
```

If NATS is down, the responder isn't running, or the signature can't be verified,
the hook prints a diagnostic to stderr and exits **non-zero** — Claude Code would
fall back to the normal prompt (never a silent allow).

## Wire it into Claude Code

The hook is delivered via a `PermissionRequest` hook in your Claude Code
settings. Add this to your **project** `.claude/settings.json` (or your user
settings), adjusting the path:

```json
{
  "hooks": {
    "PermissionRequest": [
      {
        "matcher": "*",
        "hooks": [
          { "type": "command", "command": "py E:\\.....\\hook.py" }
        ]
      }
    ]
  }
}
```

The NATS server(s) and the approval timeout are read from `handler-config.json`
itself (optional top-level keys; the registration handler preserves them):

```json
{
  "v": 1,
  "servers": "nats://127.0.0.1:4222",
  "timeout": 60,
  "clients": { "...": "..." }
}
```

| Key | Default | Meaning |
|-----|---------|---------|
| `servers` | `nats://127.0.0.1:4222` | NATS server(s) the hook connects to |
| `timeout` | `60` | seconds to wait for a human decision |

Only the config-file **location** is external — env `AI_REMOTE_HANDLER_CONFIG`
(or `--config`), defaulting to `approver/handler-config.json`. The same file
holds the `clients` allowlist the hook verifies replies against.

With the hook wired and a responder `serve`-ing, every permission prompt Claude
Code would show is instead answered by the remote operator.

## YubiKey / ARKG (optional)

`lib/yubikey.py` is a **standalone** library — not part of the approval flow above —
wrapping a YubiKey's ARKG support (CTAP2 `previewSign`). Full details in
[`CLAUDE.md`](CLAUDE.md) §8.

ARKG in one line: the key holds one *seed* pair; anyone with the seed **public** key
can derive unlimited fresh, mutually unlinkable public keys **offline**, while only
the authenticator can produce the matching private keys.

Requires `uv sync --extra yubikey` and a YubiKey on firmware **5.8.0+** advertising
`previewSign`.

> **On Windows, run everything below from an administrator terminal.** Unelevated
> processes cannot enumerate FIDO HID devices at all — Windows reserves them for the
> WebAuthn API — so the device list comes back empty *even with the key plugged in*,
> and you get `no FIDO authenticator found`. Elevation is also what makes the
> `previewSign` extension output come back instead of being silently dropped.
>
> Elevating drops `VIRTUAL_ENV`, so `py` would pick the *global* interpreter and fail
> with `ModuleNotFoundError: cryptography`. Name the venv interpreter explicitly:
>
> ```
> sudo .venv\Scripts\python.exe tools\yubikey_exec.py version
> ```

```python
import hashlib

from lib import yubikey

# 0. which key is this?
info = yubikey.get_device_info()
print(info.firmware_version, info.supports_preview_sign, info.meets_arkg_firmware)

# 1. makeCredential with previewSign -> an ARKG seed key on the device (needs a touch)
result = yubikey.make_credential()

# 2. offline: derive a fresh public key + the args the authenticator needs to sign
derived = yubikey.seed_public_key(result, ctx=b"my-purpose")   # ikm: 32 random bytes
print(derived.derived_public_key)   # verify signatures with this
print(derived.arkg_args)            # send back with result.key_handle to have it sign

# 3. optional, separate step: is this really a YubiKey?
check = yubikey.verify_yubikey_attestation(result)
print(check.is_yubikey, check.trusted_root_subject, check.reasons)

# 4. sign with the derived key (a second touch) and verify offline
signature = yubikey.sign_with_derived_key(result, derived, data=b"payload")
assert yubikey.verify_signature(derived, signature, data=b"payload")

# ... or when only the hash is at hand, on either side:
digest = hashlib.sha256(b"payload").digest()
signature = yubikey.sign_with_derived_key(result, derived, digest=digest)
assert yubikey.verify_signature(derived, signature, digest=digest)
```

> **The hashing rule.** What the authenticator signs is the **SHA-256 digest**, as-is —
> it does not hash again. So `data=` means "hash this for me", `digest=` means "this is
> already the hash" (32 bytes). Both accept the same signature; internally the digest
> path uses `Prehashed`, because hashing a digest a second time would never verify.

`is_yubikey` requires all three of: the attestation statement verifying, the
certificate chain pinning to a bundled Yubico root (`lib/yubico-fido-ca.pem`), and the
certificate naming Yubico. `reasons` says what failed.

### The `yubikey-exec` utility

`tools/yubikey_exec.py` is the command line over that library:

```bash
py tools/yubikey_exec.py version                                # no touch
py tools/yubikey_exec.py make-credential --out cred.json        # touch
py tools/yubikey_exec.py derive --in cred.json --ctx my-purpose # no device at all
py tools/yubikey_exec.py run --ctx my-purpose                   # both in one go

# sign a string with the derived key (touch), verified on the spot:
py tools/yubikey_exec.py sign --in cred.json --message "hello"
# ... or sign a ready-made hash:
py tools/yubikey_exec.py sign --in cred.json --digest <64 hex chars>

# check a signature later, with no device — note the ikm that `sign` printed:
py tools/yubikey_exec.py verify --in cred.json --ikm <hex> \
    --message "hello" --signature <hex>
```

`sign` derives, signs and verifies in one go, and prints the `ikm` it used — you need
that to reproduce the same key in `verify` (a fresh random one would give a different
key, so `--ikm` is required there). `run --message "..."` does the whole chain from
`make-credential` through sign+verify. Exit `3` means the signature did not verify,
kept distinct from `1` (error) so scripts can tell the two apart.

`make-credential --out` saves the result, so `derive` can run **later, in another
process, with the key unplugged** — that split is the whole point. Add `--json` for a
single machine-readable document, `--ikm <hex>` to reproduce a derivation.

All three hardware steps in one command file (elevate first, it does not elevate
itself — and it resolves `.venv\Scripts\python.exe` by path, since elevation drops
`VIRTUAL_ENV`):

```bat
REM in an elevated console:  sudo cmd   then:
scripts\yubikey-arkg.cmd --ctx my-purpose
scripts\yubikey-arkg.cmd --ctx my-purpose --out cred.json --attest

REM with a string to sign: step 3 becomes derive + sign + verify (second touch)
scripts\yubikey-arkg.cmd --ctx my-purpose --message "hello signed world"
```

It prints the version, asks for a touch, derives the public key, and keeps the
credential so `derive --in` can re-run without the key. Exit `0` = all steps OK.

**The "is it a YubiKey?" check is optional and off by default.** Turn it on per
invocation:

| Flag | Effect |
|------|--------|
| `--attest` | run the check and print the verdict; exit code unchanged |
| `--require-yubikey` | implies `--attest`, and exits `2` unless it verifies |
| `--roots PEM` | pin against your own trust anchors instead of the bundled ones |
| `--no-require-root` | skip chain pinning (statement + Yubico name only) |
| `--seed-attestation` | check the seed key's attestation instead of the credential's |

Exit codes: `0` ok, `1` error (no key, no `fido2`, bad input), `2` not a YubiKey while
`--require-yubikey`.

### Integration tests with a real key

Optional — skipped unless the hardware is actually there, so a plain `py -m pytest`
stays green on any machine. Touch-requiring tests are additionally env-gated so an
unattended run never blocks waiting for a finger:

```powershell
# Windows: administrator terminal. -s is required, or the touch prompt is swallowed.
$env:AI_REMOTE_YUBIKEY_TOUCH="1"; py -m pytest tests/test_yubikey_integration.py -v -s
```

A full pass asks for **two** button presses. Without the env var you still get the
read-only tier (enumeration, firmware, `getInfo`) if a key is plugged in.

## Command reference

| Command | What it does |
|---------|--------------|
| `py -m pytest -q` | Run the test suite |
| `uv sync --extra yubikey` | Install the optional `fido2` dependency |
| `py tools/yubikey_exec.py version` | YubiKey firmware / AAGUID / `previewSign` support (no touch) |
| `py tools/yubikey_exec.py make-credential --out cred.json` | ARKG seed key on the device (touch), saved for later |
| `py tools/yubikey_exec.py derive --in cred.json` | Derive a public key + `arkg_args` offline (no device) |
| `py tools/yubikey_exec.py run [--attest]` | make-credential + derive, optionally with the YubiKey check |
| `py tools/yubikey_exec.py sign --in cred.json --message "..."` | Sign a string with the derived key (touch), verify it on the spot |
| `py tools/yubikey_exec.py verify --in cred.json --ikm <hex> ...` | Check a signature offline (no device) |
| `scripts\yubikey-arkg.cmd [--message "..."]` | All hardware steps in one file (run from an elevated console) |
| `scripts\e2e-registration.cmd` | End-to-end registration check (Windows) |
| `scripts\e2e-approval.cmd` | End-to-end approval-loop check (Windows) |
| `py approver/registration_handler.py --get-token <key_id>` | Mint a one-time registration token (TTL 15 min) |
| `py approver/registration_handler.py [--once]` | Serve the `registrations` subject (allowlist owner) |
| `py approver/responder.py register <token> [--key-type ed25519\|p256]` | Generate a key pair and register its public half |
| `py approver/responder.py serve` | Answer approval requests (the human operator) |
| `py approver/hook.py` | The `PermissionRequest` hook (reads stdin, prints the decision) |

## Notes & safety

- **Runtime configs hold secrets** and are git-ignored:
  `approver/responder-config.json` (the private key) and
  `approver/handler-config.json` (live token secrets in `pending_tokens`).
- **`tool_input` travels on the bus as-is** — for `Bash` that's the full command,
  for `Write` the file contents. Restrict access to NATS and the
  `approvals.<session_id>` subject; do not connect untrusted subscribers
  (`CLAUDE.md` §7, "Privacy").
- **Run one responder at a time**, or run several under the `approvers` queue
  group so each request is answered exactly once (`CLAUDE.md` §6).
