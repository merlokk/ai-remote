# ai-remote — Claude Code permission approver over NATS

Move Claude Code's permission prompt **out of the terminal**. Instead of the
interactive "allow / deny?" prompt, a `PermissionRequest` hook publishes the
request onto [NATS](https://nats.io/), a human responder somewhere else signs
the decision with an **Ed25519** key, and the hook verifies that signature
before handing Claude Code an `allow` / `deny` verdict.

> Full protocol, message contracts and design rationale live in
> [`CLAUDE.md`](CLAUDE.md) §6–§7. This README is the practical "what it is / how
> to run it / how to check it works" guide.

## Why

The built-in permission prompt assumes the person driving Claude Code is sitting
at the terminal. This project lets a **remote human** approve (or deny)
individual tool calls:

- **Approve from elsewhere.** The decision is made by whoever is subscribed to
  the bus, not by whoever launched the session.
- **Signed & tamper-evident.** Every decision is Ed25519-signed over the exact
  command (`tool_input` hash), a per-request nonce (anti-replay) and the
  behavior. A reply that doesn't verify against a *registered* key is rejected.
- **Fail-safe by design.** NATS down, a timeout, a bad/absent signature, an
  untrusted key — **any** failure falls back to the normal interactive prompt.
  There is never a "silent allow".
- **Trust is bootstrapped, not hand-edited.** Responder keys enter the allowlist
  through a one-time-token registration flow, so no public key is ever pasted in
  by hand.

## How it fits together

```
Claude Code ──stdin──▶ hook.py ──approvals.<sid>──▶ NATS ──▶ responder.py (human: allow/deny + Ed25519 sign)
     ▲                    │                                          │
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
| `lib/crypto.py` | Ed25519 keygen / sign / verify (fail-safe verify). |
| `nats/` | `docker compose` sandbox: NATS + JetStream, a web dashboard, and `nats-box` (the `nats` CLI). |

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
```

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

### 2. Run the registration end-to-end check (Windows)

Exercises the full token → register → allowlist path against a **live NATS**,
using throwaway configs in `%TEMP%` (the repo is left untouched). Exit `0` =
PASS, `1` = FAIL.

```bat
scripts\e2e-registration.cmd
```

### 3. Smoke-test the full approval loop by hand

This walks the whole flow **without Claude Code** — you play Claude by piping a
fake `PermissionRequest` into the hook. Use three terminals.

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

# register: generates an Ed25519 pair, stores it in approver/responder-config.json,
# and adds the public key to approver/handler-config.json -> clients["approver-1"]
py approver/responder.py register "approver-1.<secret>"
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
          { "type": "command", "command": "py E:\\.....\\approver\\hook.py" }
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

## Command reference

| Command | What it does |
|---------|--------------|
| `py -m pytest -q` | Run the test suite |
| `scripts\e2e-registration.cmd` | End-to-end registration check (Windows) |
| `py approver/registration_handler.py --get-token <key_id>` | Mint a one-time registration token (TTL 15 min) |
| `py approver/registration_handler.py [--once]` | Serve the `registrations` subject (allowlist owner) |
| `py approver/responder.py register <token>` | Generate a key pair and register its public half |
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
