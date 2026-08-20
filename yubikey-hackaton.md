# ai-remote — hardware-signed permissions for Claude Code

Claude Code asks *"allow this command?"* in the terminal. This project moves that
question onto a message bus and answers it with a **YubiKey touch**, then
cryptographically verifies the answer before the tool runs.

> This is the hackathon short form. The full protocol, contracts and design
> rationale are in [`README.md`](README.md) and the per-folder
> `CLAUDE.md` files ([`CLAUDE.md`](CLAUDE.md) §2 maps them).

## The problem & the solution

**Problem.** An AI agent's permission prompt is only as trustworthy as the
terminal it's printed in. Whoever (or *whatever*) can write to that TTY can
approve a `rm -rf`. The approval is a keystroke: unattributable, unauditable,
unbound to the command it approved, and it requires the human to be sitting in
front of the session.

**Solution.** Replace the prompt with a signed request-reply round trip:

```
Claude Code ──stdin──▶ hook.py ──approvals.<session>──▶ NATS ──▶ responder (human)
     ▲                    │                                        │  touch YubiKey
     └──── allow/deny ────┘◀────────── signed reply ───────────────┘  → ECDSA P-256 sig
                          verifies sig against a registered key
```

- A `PermissionRequest` hook publishes the tool call onto [NATS](https://nats.io/).
- A remote human approves — and the decision is signed **inside a YubiKey**.
- The signature covers the *exact* command (`sha256(tool_input)`), a per-request
  nonce, the verdict and the timestamp. It cannot be replayed onto another
  command, another session, or flipped from deny to allow.
- Responder keys reach the allowlist through a one-time-token registration flow,
  never by hand-pasting a public key. That flow is authenticated both ways: the
  token proves the approver, and the registration handler signs every reply with
  its own Ed25519 key, which each approver pins on first registration.
- **Fail-safe:** bus down, timeout, bad signature, unknown key, missed touch →
  Claude Code falls back to its own prompt. There is never a silent allow.

**Why this repo exists.** Two goals, both met:

1. Prove the flow end to end as a PoC — a hardware-gated permission approver for
   an AI coding agent.
2. Write **not a single line by hand** — every line of code, every test, every
   script and both READMEs were written by Claude Code. That is the second
   result on show here: an AI agent can drive a YubiKey end to end, from
   `makeCredential` and ARKG derivation to attestation-chain pinning and a
   verified hardware signature, without a human editing the code.

## How we use the YubiKey 5.8 feature

The 5.8 firmware feature used here is **ARKG** (Asynchronous Remote Key
Generation), exposed through the CTAP2 **`previewSign`** extension — available
only on firmware **5.8.0+**.

ARKG in one line: the key holds one *seed* pair; anyone with the seed **public**
key can derive unlimited fresh, mutually unlinkable public keys **offline**,
while only the authenticator can ever produce the matching private halves.

That maps onto the approver flow exactly:

| Step | What ARKG gives us | Device? | Touch? |
|------|--------------------|---------|--------|
| `makeCredential` with `previewSign.generateKey` | an ARKG seed key on the device | yes | one |
| derive a fresh key with `(ikm, ctx="ai-remote-approvals")` | the responder's approval key — **public half derived on the host, private half nowhere** | no | none |
| register the derived public key | allowlist entry, `key_type=p256` | no | none |
| `serve` startup re-derivation | proves the config still addresses the registered key | no | none |
| each allow / deny | `previewSign.signByCredential` — the device rebuilds the private half from the key handle and signs | yes | **one per decision** |

Consequences we cared about:

- **No private key on the host.** `responder-yubikey-config.json` holds only the
  credential, `ctx` and `ikm`. Leaking it leaks no signing capability.
- **Every decision costs a physical touch.** No touch → no reply → fall back to
  the local prompt.
- **Nothing on the verifying side knows about YubiKeys.** An ARKG derived key
  *is* a P-256 key and the device signs ECDSA-P256/SHA-256 DER, so the hook
  verifies a hardware reply through the same code path as a software one. The
  only glue needed was COSE `x`/`y` → compressed SEC1 point.
- **Unlinkability is enforced, not suggested.** `ikm` must be ≥ 32 bytes;
  re-registering the same key yields an unrelated public key.
- **"Is it really a YubiKey?"** is an opt-in attestation check
  (`--require-yubikey`): statement verification + chain pinning to a bundled
  Yubico FIDO root + a Yubico name in the cert. Real keys ship only the
  end-entity cert in `x5c`, so the bundled Yubico *intermediates* are spliced in
  to build the path — without them a genuine 5.8 key reports a false negative.

## Setup & run

Prerequisites: **Docker Desktop**, **Python 3.14** (`py` launcher),
[**uv**](https://docs.astral.sh/uv/), and a **YubiKey 5.8.0+** advertising
`previewSign`.

```bash
uv sync --extra yubikey          # deps + the fido2 extra (YubiKey path)
cd nats && docker compose up -d  # NATS + JetStream + dashboard + nats-box
cd ..
py -m pytest -q                  # full suite, no hardware needed
```

Check the key is seen and speaks ARKG — **Windows needs an administrator
terminal** (unelevated processes can't enumerate FIDO HID devices, and the
`previewSign` output is dropped), and elevation drops `VIRTUAL_ENV`, so name the
venv interpreter by path:

```bat
sudo .venv\Scripts\python.exe tools\yubikey_exec.py version
```

The whole loop — token, credential, registration, responder, hook, verdict — in
one command file (six steps, two touches: one `makeCredential`, one decision):

```bat
REM from an elevated console (sudo cmd), NATS up:
scripts\yubikey-approval.cmd --require-yubikey
scripts\yubikey-approval.cmd --deny                  REM exercise the deny path
scripts\yubikey-approval.cmd --credential cred.json  REM reuse a credential: one touch
```

Exit `0` = PASS, `1` = FAIL, `2` = `--require-yubikey` said no. Or drive the
pieces by hand:

```bat
py  approver\registration_handler.py --get-token approver-yk
sudo .venv\Scripts\python.exe approver\responder_yubikey.py register approver-yk.<secret> --require-yubikey
sudo .venv\Scripts\python.exe approver\responder_yubikey.py serve
```

Then wire it into Claude Code's `settings.json` as a `PermissionRequest` hook
(matcher `*`, command `py <repo>\approver\hook.py`) and every permission prompt
becomes a YubiKey touch. There is also a software responder with the key on disk
(`approver/responder.py`) — it is **not** the intended way to run this, it exists
so the protocol can be tested and developed without hardware; it and the
`yubikey-exec` CLI are covered in [`README.md`](README.md).

## Screenshots

The whole run, captured step by step, is in [`screens/`](screens/):

1. [NATS up on Docker](screens/1.%20docker%20NATS.png)
2. [the hook wired into Claude Code](screens/2.%20hook%20in%20claude.png)
3. [the key seen, firmware and `previewSign`](screens/3.%20key%20version.png)
4. [minting a one-time registration token](screens/4.%20generate%20one-time%20code.png)
5. registration — [client](screens/5.%20registration%20-%20client.png) / [server](screens/5.%20registration%20-%20server.png)
6. [an approved request](screens/6.%20approved%20request.png)

## Tech stack & dependencies

| | |
|---|---|
| Language | Python 3.14 (stdlib-first; no web framework, no ORM, no CLI lib) |
| Transport | **NATS** core request-reply (`approvals.<session_id>`, `registrations`), JetStream enabled in the sandbox |
| Crypto | ECDSA **P-256**/SHA-256 (YubiKey ARKG keys) and Ed25519 (software responder) |
| Hardware | YubiKey 5.8.0+, CTAP2 **`previewSign`** (ARKG) — DRAFT/experimental upstream |
| Infra | Docker Compose: NATS server, web dashboard, `nats-box` CLI |
| Packaging | **uv** (`pyproject.toml` + committed `uv.lock`, non-package project) |
| Tests | **pytest** — pure-logic, fake-hardware, and opt-in real-key tiers |

Dependencies, deliberately four:

- `nats-py` — NATS client
- `cryptography` — Ed25519 / ECDSA P-256 sign & verify
- `fido2` — CTAP2 / `previewSign`; **optional extra**, so the core approval path
  keeps working on a bare `uv sync`
- `pytest` — dev only

The YubiKey code is fully testable **without** a YubiKey: the ARKG seed key is
assembled synthetically in the real COSE shape (real curve math) and attestation
runs against a throwaway CA, so `py -m pytest` is green on any machine.
Real-hardware tests are a separate, env-gated tier.
