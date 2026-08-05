# ai-remote — Claude Code permission approver over NATS

Move Claude Code's permission prompt **out of the terminal**. Instead of the
interactive "allow / deny?" prompt, a `PermissionRequest` hook publishes the
request onto [NATS](https://nats.io/), a human responder somewhere else signs the
decision **on a YubiKey** (ARKG / ECDSA P-256), and the hook verifies that
signature before handing Claude Code an `allow` / `deny` verdict. A software
responder (P-256 or Ed25519 on disk) exists for tests and for developing without
hardware.

> Full protocol, message contracts and design rationale live in
> [`CLAUDE.md`](CLAUDE.md) §6–§7. This README is the practical "what it is / how
> to run it / how to check it works" guide.

## Why

The built-in permission prompt assumes the person driving Claude Code is sitting
at the terminal. This project lets a **remote human** approve (or deny)
individual tool calls:

- **Approve from elsewhere.** The decision is made by whoever is subscribed to
  the bus, not by whoever launched the session.
- **A physical human, by default.** The primary responder keeps its signing key
  inside a YubiKey, so every allow / deny costs a touch and no private key sits
  on any host.
- **Signed & tamper-evident.** Every decision is signed (ECDSA P-256 or Ed25519,
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
Claude Code ──stdin──▶ hook.py ──approvals.<sid>──▶ NATS ──▶ responder_yubikey.py
     ▲                    │                                         │  (human: allow/deny
     └──── allow/deny ────┘◀──────────── signed reply ──────────────┘   + touch → ARKG P-256 sign)
                          (hook verifies sig against the allowlist)

registration_handler.py  ──▶ owns handler-config.json (the allowlist `clients` + one-time `pending_tokens`)
responder.py             ──▶ software stand-in for the same role, key on disk (Ed25519/P-256):
                             used by the automated e2e checks and for hardware-free development
```

**`responder_yubikey.py` is the responder this project is about** — the signing
key lives on a YubiKey, so there is no private key on disk and each decision
costs a physical touch. `responder.py` is the same protocol with a software key:
it exists so the flow can be tested and developed without hardware in the loop
(and it is what `scripts\e2e-approval.cmd` drives). Both are indistinguishable to
`hook.py`.

| Component | Role                                                                                                                                                                           |
|-----------|--------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `approver/hook.py` | Claude Code `PermissionRequest` hook. Sends the request, verifies the signed reply against the allowlist, prints the decision. Fail-safe.                                      |
| `approver/responder_yubikey.py` | **The primary responder.** The human side with the key on a **YubiKey**: no private key on disk, every decision costs a touch. Needs the `yubikey` v5.8 extra.                 |
| `approver/responder.py` | The software stand-in for the same role (key on disk, Ed25519/P-256) — for tests, the e2e scripts, and hardware-free development.                                              |
| `approver/registration_handler.py` | Owns the allowlist (`handler-config.json`). Mints one-time tokens and registers responder public keys.                                                                         |
| `approver/protocol.py` | Shared wire-format: canonical JSON, hashes, and the exact "signing bytes" both sides assemble identically.                                                                     |
| `lib/bus.py` | Thin async JSON request-reply wrapper over `nats-py`.                                                                                                                          |
| `lib/config.py` | Versioned, atomic JSON config store.                                                                                                                                           |
| `lib/crypto.py` | Ed25519 / ECDSA P-256 keygen / sign / verify (fail-safe verify; scheme picked by `key_type`).                                                                                  |
| `lib/yubikey.py` | **Optional:** YubiKey ARKG (`previewSign`) — version, `makeCredential`, offline key derivation, signing, "is it a YubiKey?" attestation. Needs the `yubikey` extra.            |
| `nats/` | `docker compose` sandbox: NATS + JetStream, a web dashboard, and `nats-box` (the `nats` CLI).                                                                                  |
| `scripts/` | Windows command-file end-to-end checks: `e2e-registration.cmd` (§6), `e2e-approval.cmd` (§7), plus the hardware runs `yubikey-arkg.cmd` (§8) and `yubikey-approval.cmd` (§8.7). |

## Prerequisites

- **Docker Desktop** (for the NATS sandbox).
- **Python 3.14** via the **`py`** launcher (see `CLAUDE.md` §5).
- **[uv](https://docs.astral.sh/uv/)** for dependency management.
- A **YubiKey on firmware 5.8.0+** advertising `previewSign` — for the primary
  (hardware) responder. Everything else runs without one.

## Setup

```bash
# 1. Install dependencies (runtime + dev + the fido2 extra) into a local .venv
uv sync --extra yubikey

# 2. Bring up the NATS sandbox (server + dashboard + nats-box)
cd nats && docker compose up -d && cd ..

# No YubiKey at hand? The protocol core needs no extra:
uv sync
```

Install the extra for the normal (YubiKey) setup — see
[The YubiKey-backed responder](#the-yubikey-backed-responder-the-primary-one). It is
still an **optional** extra by design: the protocol core (`hook.py`, `responder.py`,
`registration_handler.py`) never imports `fido2`, so the hardware-free flow below
works on a bare `uv sync`. Without the extra,
`lib/yubikey.py` still imports and its pure helpers work; its device features raise a
clear error and its tests skip.

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

This walks the whole flow **without Claude Code and without hardware** — you play
Claude by piping a fake `PermissionRequest` into the hook, and the software
`responder.py` stands in for the YubiKey one. Use three terminals. (For the real
thing, jump to
[The YubiKey-backed responder](#the-yubikey-backed-responder-the-primary-one).)

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
Code would show is instead answered by the remote operator — with
`responder_yubikey.py serve`, by a touch on their YubiKey.

## YubiKey / ARKG

`lib/yubikey.py` wraps a YubiKey's ARKG support (CTAP2 `previewSign`). It is usable on
its own, and it is what backs the primary responder further down. Full details in
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

The whole hardware flow in one command file — **five steps**: version →
make-credential (touch) → derive → sign (touch) → verify offline. Elevate first, it
does not elevate itself, and it resolves `.venv\Scripts\python.exe` by path since
elevation drops `VIRTUAL_ENV`:

```bat
REM in an elevated console:  sudo cmd   then:
scripts\yubikey-arkg.cmd
scripts\yubikey-arkg.cmd --ctx my-purpose --message "hello signed world"
scripts\yubikey-arkg.cmd --ctx my-purpose --out cred.json --attest
scripts\yubikey-arkg.cmd --no-sign          REM stop after derive; one touch only
```

Two touches: one for `make-credential`, one for `sign`. The script mints a single
`ikm` up front and passes it to steps 3–5 so they all address the *same* derived key,
and hands the signature from step 4 to step 5 through a file (`--sig-out`). Step 5 is
the point of it: it re-derives the public key from the credential file alone and
verifies with **no device attached**.

Credential and signature files are kept (paths printed at the end) so `derive` and
`verify` can re-run later. Exit `0` = all steps OK, `1` = a step failed, `3` = the key
signed but the signature does not verify.

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

### The YubiKey-backed responder (the primary one)

`approver/responder_yubikey.py` is **the responder this project exists for** — the
software `responder.py` above is its hardware-free stand-in for tests and
development. Same subjects, same wire format, but **no private key on this
machine** and one touch per decision. The key it signs with is an ARKG key derived
from the authenticator's seed key, so the private half only ever exists inside the
device.

`hook.py` needs no changes and knows nothing about YubiKeys: an ARKG derived key *is* a
P-256 key, and the authenticator signs ECDSA-P256 over SHA-256, DER-encoded — exactly
`lib/crypto.py`'s `key_type: "p256"` scheme. Registration publishes the derived key as
the compressed SEC1 point that scheme expects, so a YubiKey reply is verified through
the same code path as a software one.

```bat
REM 1. mint a token as usual (the allowlist owner does this)
py approver\registration_handler.py --get-token approver-yk

REM 2. register: makeCredential on the key (one touch), derive, publish the derived
REM    public key with key_type=p256. Elevated console, venv interpreter by path:
sudo .venv\Scripts\python.exe approver\responder_yubikey.py register approver-yk.<secret> --require-yubikey

REM ... or reuse a credential saved earlier — no device, no touch at all:
py approver\responder_yubikey.py register approver-yk.<secret> --credential cred.json

REM 3. serve: prompt the operator, then sign each decision on the key
sudo .venv\Scripts\python.exe approver\responder_yubikey.py serve
```

Its prompt is a single `a`/`d`/`s` keystroke — unlike the software responder it does
**not** ask for a free-text `reason`: the decision already costs a physical touch, and a
question between the keystroke and the touch is one step too many. `reason` still goes
on the wire (empty) and is still covered by the signature. Once the key has signed, the
outcome is echoed back — neither the keystroke nor the touch tells you what actually
left the machine:

```
allow / deny / skip? [a/d/s]: a

>>> touch your YubiKey to sign this decision <<<
  decision  : allow
  signature : sha256:7faff4db5669b2433dc98cd9f75df674ee998f265b21d1c8670c7a3e1dfda221
  sent to the hook
```

`register` writes `approver/responder-yubikey-config.json` only after the handler acks,
same as the software responder. That file holds **no private key** — just the
credential, the derivation label `ctx` and the `ikm`. That is enough to re-derive the
*public* key offline and to let the device rebuild the private half when it signs.
`serve` re-derives at startup and refuses to start if the result no longer matches the
registered public key: such a responder would only produce replies the hook must reject.

| Step | Device? | Touch? |
|------|---------|--------|
| `register` (no `--credential`) | yes | one — `makeCredential` |
| `register --credential FILE` | no | none — derivation is offline |
| `serve` startup | no | none — re-derivation is offline |
| each allow / deny | yes | one — the decision is signed on the key |

The whole loop in one command file — six steps, two touches. Elevate first; it does
not elevate itself, and it resolves `.venv\Scripts\python.exe` by path:

```bat
REM in an elevated console:  sudo cmd   then:
scripts\yubikey-approval.cmd --require-yubikey
scripts\yubikey-approval.cmd --deny                  REM exercise the deny path
scripts\yubikey-approval.cmd --credential cred.json  REM reuse a credential: one touch
```

It mints a throwaway token, makes the credential (**touch**), registers the derived
key through the real registration handler, starts a real `responder_yubikey serve` in
its own window, pipes a `PermissionRequest` into the real `hook.py` (**touch**), and
checks the decision that comes back. Along the way it prints the allowlist entry the
handler stored, so you can see the `key_type=p256` + compressed point the hook verifies
with. Only the operator's `a`/`d` keystroke is scripted; the touch is not. The
credential is kept (path printed), so a re-run with `--credential` costs one touch
instead of two. Exit `0` = PASS, `1` = FAIL (each failure prints its likely cause and
`hook.py`'s own reason verbatim), `2` = `--require-yubikey` said no.

The "is it a YubiKey?" check is opt-in here too: `--attest` reports the verdict,
`--require-yubikey` refuses to register anything that does not pin to a Yubico root
(exit `2`, nothing published, nothing persisted). Worth using — registration is the
moment you choose which key to trust.

Caveats, all inherited from §8 or §6:

- **Administrator terminal on Windows**, or the `previewSign` output is silently dropped;
  elevation drops `VIRTUAL_ENV`, so name `.venv\Scripts\python.exe` by path.
- `--rp-id` must match the credential — it defaults to `example.com` on both sides, and
  is stored in the config so `serve` picks it up by itself.
- Registering the same YubiKey twice gives two **unlinkable** keys: the `ikm` is fresh
  each time. Re-registering rotates `clients[key_id]` in the allowlist.
- **Run one responder at a time.** Both responders share the `approvers` queue group,
  so with both running each request goes to exactly one of them — but which one is
  arbitrary.
- Key unplugged, or a touch that never comes: no reply is sent, and Claude Code falls
  back to its own prompt (§7 fail-safe). It never becomes a silent allow.

### Integration tests with a real key

Optional — skipped unless the hardware is actually there, so a plain `py -m pytest`
stays green on any machine. Touch-requiring tests are additionally env-gated so an
unattended run never blocks waiting for a finger:

```powershell
# Windows: administrator terminal. -s is required, or the touch prompt is swallowed.
$env:AI_REMOTE_YUBIKEY_TOUCH="1"; py -m pytest tests/test_yubikey_integration.py -v -s
```

A full pass asks for **several** button presses — one shared `makeCredential` plus one
per signature, including the YubiKey-responder round trip that ends in
`hook.verify_reply`. The prompts say which step is asking. Without the env var you still
get the read-only tier (enumeration, firmware, `getInfo`) if a key is plugged in.

## Command reference

| Command | What it does |
|---------|--------------|
| `py -m pytest -q` | Run the test suite |
| `uv sync --extra yubikey` | Install everything incl. the `fido2` extra (the YubiKey path) |
| `py tools/yubikey_exec.py version` | YubiKey firmware / AAGUID / `previewSign` support (no touch) |
| `py tools/yubikey_exec.py make-credential --out cred.json` | ARKG seed key on the device (touch), saved for later |
| `py tools/yubikey_exec.py derive --in cred.json` | Derive a public key + `arkg_args` offline (no device) |
| `py tools/yubikey_exec.py run [--attest]` | make-credential + derive, optionally with the YubiKey check |
| `py tools/yubikey_exec.py sign --in cred.json --message "..."` | Sign a string with the derived key (touch), verify it on the spot |
| `py tools/yubikey_exec.py verify --in cred.json --ikm <hex> ...` | Check a signature offline (no device) |
| `scripts\yubikey-arkg.cmd` | All five hardware steps in one file: version, make-credential, derive, sign, verify (elevated console) |
| `scripts\yubikey-approval.cmd` | The whole YubiKey approval loop: token, make-credential, register, serve, hook, verify (elevated console, two touches) |
| `scripts\e2e-registration.cmd` | End-to-end registration check (Windows) |
| `scripts\e2e-approval.cmd` | End-to-end approval-loop check (Windows) |
| `py approver/registration_handler.py --get-token <key_id>` | Mint a one-time registration token (TTL 15 min) |
| `py approver/registration_handler.py [--once]` | Serve the `registrations` subject (allowlist owner) |
| `py approver/responder_yubikey.py register <token> [--credential FILE] [--require-yubikey]` | **Primary:** register an ARKG key derived from a YubiKey (`key_type=p256`) |
| `py approver/responder_yubikey.py serve` | **Primary:** answer approval requests, signing each decision on the YubiKey (elevated console) |
| `py approver/responder.py register <token> [--key-type ed25519\|p256]` | Software stand-in: generate a key pair and register its public half |
| `py approver/responder.py serve` | Software stand-in: answer approval requests with the on-disk key (tests / no hardware) |
| `py approver/hook.py` | The `PermissionRequest` hook (reads stdin, prints the decision) |

## Notes & safety

- **Runtime configs hold secrets** and are git-ignored:
  `approver/responder-config.json` (the private key) and
  `approver/handler-config.json` (live token secrets in `pending_tokens`).
  `approver/responder-yubikey-config.json` is git-ignored too — it holds no private key
  (that never leaves the YubiKey), but it identifies the credential behind a
  registered key.
- **`tool_input` travels on the bus as-is** — for `Bash` that's the full command,
  for `Write` the file contents. Restrict access to NATS and the
  `approvals.<session_id>` subject; do not connect untrusted subscribers
  (`CLAUDE.md` §7, "Privacy").
- **Run one responder at a time**, or run several under the `approvers` queue
  group so each request is answered exactly once (`CLAUDE.md` §6).
