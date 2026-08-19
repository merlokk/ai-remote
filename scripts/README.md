# `scripts/` — the command files

Windows command files that drive the flows end to end. Two are self-checking
tests (they pass or fail on their own); four need a human, to press a YubiKey, to
press a button on the ESP32, or to read what came back.

This is the operational guide: what each one does, how to start it, and what its
exit code means. The design rationale and the gotchas behind each script live
next to the flow it drives — [`approver/CLAUDE.md`](../approver/CLAUDE.md) for
the approval and registration ones, [`tools/CLAUDE.md`](../tools/CLAUDE.md) for
the two that wrap `tools/`.

| Script | What it does | Self-checking? | Needs hardware? | Elevated? |
|--------|--------------|----------------|-----------------|-----------|
| [`e2e-registration.cmd`](#e2e-registrationcmd) | registers a responder against a real handler and checks the allowlist | yes | no | no |
| [`e2e-approval.cmd`](#e2e-approvalcmd) | a full allow decision through real `responder.py` + `hook.py` | yes | no | no |
| [`test-request.cmd`](#test-requestcmd) | sends one permission request and prints the answer | no — you read it | no | no |
| [`yubikey-arkg.cmd`](#yubikey-arkgcmd) | the five-step ARKG hardware run: version → credential → derive → sign → verify | no — reports what the key returned | **YubiKey** (2 touches) | **yes** |
| [`yubikey-approval.cmd`](#yubikey-approvalcmd) | the whole approval loop with the key on a YubiKey | yes, apart from the touches | **YubiKey** (2 touches) | **yes** |
| [`esp32-approval.cmd`](#esp32-approvalcmd) | the whole approval loop against the ESP32 on the desk | yes, apart from the presses | **the board** (1 press, 1 deliberate non-press) | no |

## Before you start

**NATS must be up** for every script except a `--dry-run`:

```bat
cd nats && docker compose up -d && cd ..
```

**`uv sync` must have been run**, and that is now the whole requirement. Every
script here resolves `.venv\Scripts\python.exe` **by path** and falls back to the
`py` launcher with a warning if it is not there.

That used to be true of only three of them, and the other two were a trap worth
recording rather than quietly deleting: `py` finds the venv only when
`VIRTUAL_ENV` happens to be set in the calling shell, so the two `e2e-*` scripts
worked from a venv-activated console and died from an ordinary one with
`ModuleNotFoundError: No module named 'nats.aio'` — or, worse, with nothing but
`could not mint token`, which reads as a NATS problem. The YubiKey scripts had
already been made to resolve the interpreter by path for a *different* reason
(elevation drops `VIRTUAL_ENV`), so the fix was to spread a decision that had
been taken twice already rather than to invent one.

**The YubiKey scripts do not elevate themselves.** Start an elevated console
first (`sudo cmd`, or "Run as administrator") and run the script from there.
Unelevated, Windows hands FIDO devices to the WebAuthn API and the `previewSign`
output is dropped — see [`lib/CLAUDE.md`](../lib/CLAUDE.md) §8.

**Nothing here touches your real config.** Every script works in throwaway
configs under `%TEMP%`; the exception is `test-request.cmd`, which reads (never
writes) `approver\handler-config.json` to judge the reply.

---

## `e2e-registration.cmd`

The §6 registration flow end to end: mint a one-time token → bring the handler
up with `--once` → `responder.py register` → check what landed in the allowlist.

```bat
scripts\e2e-registration.cmd
```

No arguments. It verifies three things: `clients[key_id].pubkey` is exactly the
responder's `public_key`, the token was spent (`pending_tokens` is empty), and
the handler's `server_key.public_key` is the one the responder pinned.

A pass looks like this:

```
[1/4] minting one-time token for approver-e2e ...
   token: approver-e2e.96+b/MvGR0sBCiwipZ7lbjfFjzpR0/db8Bue3Isgro8=
[2/4] starting registration handler (--once, exits after first success) ...
[3/4] registering responder (retry until the handler is subscribed) ...
   registered.
[4/4] verifying handler allowlist against responder key ...
    handler pubkey : pc5NsS2/LXoFH5B1ekP7vssEV4p38tDWbCPXPzVOqCU=
    responder pub  : pc5NsS2/LXoFH5B1ekP7vssEV4p38tDWbCPXPzVOqCU=
    pending_tokens : []
    server key     : S0NnyRwZ8krAg0P34tELhN7+sfPKDM5Yg++QL+x/xeo=
    pinned by resp : S0NnyRwZ8krAg0P34tELhN7+sfPKDM5Yg++QL+x/xeo=

==== E2E PASSED ====
```

**Exit codes:** `0` PASS, `1` FAIL.

## `e2e-approval.cmd`

The §7 approval loop end to end, as real processes: register a responder → start
`responder.py serve` in the background → pipe a `PermissionRequest` into
`hook.py` → check the hook printed a signed `allow`.

```bat
scripts\e2e-approval.cmd
```

No arguments. The operator's `allow` keystroke is fed from a redirected answers
file rather than typed, so it runs unattended; everything else is the real thing,
including stdin/stdout and the exit codes.

**Exit codes:** `0` PASS, `1` FAIL.

## `test-request.cmd`

Sends **one** permission request to whatever responder is listening and prints
the answer, judged by `hook.verify_reply` against the same allowlist Claude Code
would use. So the verdict you read is the verdict Claude Code would act on.

Use it to exercise a responder without a live Claude Code session — the web page,
`responder.py serve` and `responder_yubikey.py serve` all look the same from this
side — and to make the web UI's alert fire on demand.

```bat
scripts\test-request.cmd                                  REM the default echo command
scripts\test-request.cmd --command "rm -rf build"
scripts\test-request.cmd --timeout 120 --json             REM machine-readable
scripts\test-request.cmd --dry-run --command "rm -rf b"   REM show the request, send nothing
scripts\test-request.cmd --help                           REM authoritative: every option is forwarded
```

| Option | Meaning |
|--------|---------|
| `--command TEXT` | the Bash command to ask about (the common case) |
| `--tool NAME` | tool name (default `Bash`) |
| `--input JSON` | full `tool_input` as a JSON object; overrides `--command` |
| `--session ID` | session id (default: a fresh `test-request-<hex>`) |
| `--timeout SEC` | how long to wait for a decision (default: the config's) |
| `--config FILE` | handler config to verify against (default `approver\handler-config.json`) |
| `--json` | print one JSON document instead of a report |
| `--dry-run` | print the request that would go out; no NATS, no operator |

A trusted answer prints both digests as well as the verdict:

```
-> approvals.test-request-4d7c7083  (Bash)
   waiting up to 60s for a responder
  behavior  : allow
  reason    : ''
  key_id    : approver-web
  input sha : b3eba939fcc8249aa7fe19229471bac68ae4b27571039768f72d2d6eb9726edb
  signature : sha256:f03ef72d0c874e7f3efb8171e286fc4c4fd6c85999b4e7cd65d5bbdf8a0f1aef (72 bytes)
  verdict   : TRUSTED - Claude Code would allow this
```

The `signature :` line is in the **same shape `responder_yubikey.py serve`
prints on the operator's console**. Comparing the two character for character is
the only way to confirm that the decision which reached the hook is the one the
operator watched themselves sign.

**`--input` quoting is opposite in the two shells** — each shell's working form
is the other's argparse error, because PowerShell will not pass the
escaped-double-quote form to a native process and `cmd.exe` does not treat single
quotes as quoting at all:

```powershell
# PowerShell
scripts\test-request.cmd --tool Write --input '{"file_path": "x.txt"}'
```
```bat
REM cmd.exe
scripts\test-request.cmd --tool Write --input "{\"file_path\": \"x.txt\"}"
```

When in doubt add `--dry-run`: it prints the request including the parsed
`tool_input` and its `input_sha256`, which is the fastest way to tell a quoting
problem from a responder problem.

**Exit codes:** `0` a trusted reply arrived, `1` error or nobody answered (check
that NATS and a responder are up), `3` a reply arrived that the hook would
**reject** — usually an unregistered responder, or the wrong `--config`.

## `yubikey-arkg.cmd`

The §8 ARKG flow against real hardware, in five steps: `version` →
`make-credential` (**first touch**) → `derive` → `sign` (**second touch**) →
`verify` (offline, no device). Not a self-checking test — a human finger is in
the loop; it drives the real `tools/yubikey_exec.py` and reports what the key
returned.

```bat
REM from an already-elevated console (sudo cmd):
scripts\yubikey-arkg.cmd
scripts\yubikey-arkg.cmd --attest                    REM also: is this really a YubiKey?
scripts\yubikey-arkg.cmd --no-sign                   REM stop after derive: one touch
scripts\yubikey-arkg.cmd --message "sign this"
scripts\yubikey-arkg.cmd --out cred.json             REM keep the credential here
```

| Option | Meaning |
|--------|---------|
| `--ctx LABEL` | derivation context label (default `ai-remote`) |
| `--out FILE` | where to keep the credential (default: a file in `%TEMP%`) |
| `--attest` | also run the optional "is this really a YubiKey?" check |
| `--message STRING` | what to sign (default: a fixed smoke-test string) |
| `--ikm HEX` | reuse a specific `ikm` instead of a fresh random one |
| `--no-sign` | stop after step 3 — one touch, no signing |

The credential and signature files are **kept** (paths printed at the end), so
`derive` and `verify` can be re-run later with the key unplugged. Step 5 is the
meaningful one: it re-derives the public key from the credential file alone and
verifies with no device attached.

**Exit codes:** `0` all steps OK, `1` a step failed (each failure prints its
likely cause), `3` the key signed but the signature does not verify — kept
distinct from a missed touch on purpose.

## `yubikey-approval.cmd`

The §8.7 flow end to end, in six steps: throwaway handler config + token →
`make-credential` (**first touch**) → `register --credential` (no device) →
`responder_yubikey serve` (offline re-derivation, own window) → `hook.py` fed a
`PermissionRequest` (**second touch**) → check the printed decision.

Real processes over real NATS. Only the operator's `a`/`d` keystroke is scripted;
the touch is not, and cannot be.

```bat
REM from an already-elevated console (sudo cmd):
scripts\yubikey-approval.cmd
scripts\yubikey-approval.cmd --require-yubikey       REM refuse to register a non-YubiKey
scripts\yubikey-approval.cmd --deny                  REM exercise the deny path
scripts\yubikey-approval.cmd --credential cred.json  REM reuse a credential: one touch, not two
```

| Option | Meaning |
|--------|---------|
| `--ctx LABEL` | derivation context label (default `ai-remote-approvals`) |
| `--credential FILE` | reuse a saved credential; skips step 2 and its touch |
| `--out FILE` | where to keep the new credential (default: in `%TEMP%`) |
| `--rp-id ID` | relying party of the credential (default `example.com`) |
| `--attest` | report the "is this really a YubiKey?" check at register |
| `--require-yubikey` | imply `--attest` and refuse to register unless it verifies |
| `--deny` | answer deny instead of allow, and expect a signed deny |
| `--timeout SEC` | how long `hook.py` waits for the decision (default 120) |

`make-credential` deliberately runs **before** `register`, because
`register --credential FILE` needs no device: the touch is spent once, up front,
and a re-run with `--credential <the kept file>` costs no `makeCredential` touch
at all. Step 3 prints the allowlist entry the handler stored, so the
`key_type=p256` compressed point is visible; a non-p256 entry fails the run.

**Exit codes:** `0` PASS, `1` FAIL, `2` `--require-yubikey` said this is not a
YubiKey.

## `esp32-approval.cmd`

The device tier of [`approver-esp32/tests.md`](../approver-esp32/tests.md)
§10.11 — the acceptance test for the firmware, and the only place in this
repository where the key bound to that chip meets the allowlist Claude Code
verifies against.

```bat
scripts\esp32-approval.cmd
```

No arguments. It sets `AI_REMOTE_ESP32_DEVICE=1` and runs
`tests\test_esp32_device.py`, which asks for **two interactions** and says so on
screen: press ALLOW (the `BOOT` button) on the card that appears, then press
nothing at all for about twenty seconds. The second one is the §10.10 fail-safe —
no press must mean no reply, so that the hook times out and Claude Code falls
back to its own terminal. Everything else is derived from the one signed reply
without asking again: the verdict flipped, each echoed field changed in turn, the
`key_id` renamed, the signature removed.

**Three preconditions, and the third is the one that produces a confusing
failure:**

- NATS is up and the device is connected to it — `nats` on its console;
- the device is **registered**, so `approver-esp32` is in `handler-config.json`.
  This script does not register it: a token is one-time, and a test that spent
  one per run would need a fresh one per run. `register <token>` on the console
  ([`approver-esp32/commands.md`](../approver-esp32/commands.md));
- **nothing else is subscribed to `approvals.*`** — no `responder.py serve`, no
  browser tab. §6's queue group hands each request to exactly one responder, so
  another one running is a suite that passes against the wrong key. The test
  asserts `key_id` for precisely that reason, but stopping them first turns a
  puzzling failure into no failure at all.

Without `AI_REMOTE_ESP32_DEVICE=1` every test in the file skips, so a plain
`pytest` on a machine with no board stays green.

It resolves `.venv\Scripts\python.exe` **by path** rather than calling `py`, for
the reason the two YubiKey scripts do: it needs `cryptography` and `nats-py`, and
the launcher only finds them when a venv happens to be active.

**Exit codes:** `0` PASS, `1` FAIL (or the suite skipped).

---

## Editing these files

- **Keep them CRLF.** `yubikey-approval.cmd` broke with LF endings: `cmd`
  resumed at the wrong offset after `goto :parse` and skipped whole `if` blocks,
  so a second option died as `unknown argument`. It is offset-dependent, so the
  shorter scripts happen to get away with LF — do not rely on that.
- **Avoid parenthesised `if` blocks** where a `goto` can land inside them, for
  the same reason.
- **`python -c "..."` mangles quotes here.** Capturing a value with
  `for /f "usebackq"` on a backquoted command truncates the inner string at its
  first quote. Where a value has to travel, these scripts write it to a file and
  read it back with a plain `for /f` — that is why `--sig-out` and `--out` exist.
  **All four token-minting scripts do it that way now.** The two `e2e-*` ones
  used to capture the token from a backquoted command and got away with it
  because base64 contains no quote — which is the kind of thing that works until
  the day the format changes, and the file idiom costs one extra line.
- **Resolve the interpreter by path**, per the note above: `set "PYEXE=…"`, a
  warning, and a `py` fallback. Copy the three lines rather than reasoning about
  whether a particular script needs them.
