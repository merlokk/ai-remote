# tools/ — command-line utilities

Command-line front ends over [`lib/`](../lib/CLAUDE.md) and
[`approver/`](../approver/CLAUDE.md). Has an `__init__.py` so they are importable
and unit-testable, and each keeps its pure helpers apart from its I/O for exactly
that reason.

Neither tool reimplements protocol logic: `test_request.py` builds and judges its
request with `hook.py`'s own functions, and `yubikey_exec.py` is a CLI over
`lib/yubikey.py` and nothing more.

This file owns section **8.5** ([`../CLAUDE.md`](../CLAUDE.md) §2 has the map);
the library behind it is §8 in [`lib/CLAUDE.md`](../lib/CLAUDE.md), and the
approval protocol `test_request.py` speaks is §6–§7 in
[`approver/CLAUDE.md`](../approver/CLAUDE.md).

## Modules

- `tools/test_request.py` — a **responder-side probe**: sends one permission request and prints the answer. It reuses `hook.build_request` to build it and `hook.verify_reply` + `allowlist_from_config` to judge it, so what it sends is exactly what the hook sends and the verdict it prints is the verdict Claude Code would act on — nothing here is a reimplementation that can drift. Exercises any responder (the web UI, `responder.py serve`, `responder_yubikey.py serve`) without a live Claude Code session, and is the quickest way to make `approver-web`'s alert fire on demand. `--command TEXT` (Bash shorthand) or `--input JSON` for a full `tool_input`; `--tool` / `--session` / `--cwd` / `--mode` / `--timeout` / `--servers` / `--config` / `--json`. Prints two digests as well as the verdict: `input_sha256`, so you can see **which command** the decision is bound to rather than only that one arrived, and the signature's `sha256:<hex>` fingerprint in the **same shape `responder_yubikey.print_decision` echoes on the operator's console** — comparing those two lines character for character is the only way to confirm the decision that reached the hook is the one the operator watched themselves sign. `--dry-run` prints the request instead of sending it — no NATS, no card in front of an operator — which is how you tell a shell-quoting problem from a responder problem (see the examples under `scripts/test-request.cmd`). Pure helpers `parse_tool_input` / `build_payload` / `describe_reply` / `format_report` are tested without NATS. Exit `0` trusted reply, `1` error or nobody answered, `3` a reply arrived that the hook would reject (usually an unregistered responder, or the wrong `--config`). Run: `py tools/test_request.py --command "echo hi"`.
- `tools/yubikey_exec.py` — the **`yubikey-exec`** utility (§8.5). Six subcommands: `version` (no touch) / `make-credential` (touch; `--out` saves the result) / `derive` (no device — derives from a saved result) / `sign` (derive + a second touch + verify on the spot) / `verify` (no device — re-derives and checks a signature) / `run` (make-credential + derive at once, and sign when given something to sign). The "is it a YubiKey?" check is **opt-in**: `--attest` reports it, `--require-yubikey` also makes a negative verdict exit 2. `--json` emits one document. Pure helpers `parse_ikm` / `device_report` / `credential_report` / `derived_report` / `attestation_report` / `signature_report` / `parse_signature` / `write_signature` are tested without hardware. Run: `py tools/yubikey_exec.py <cmd>`.

### 8.5 The `yubikey-exec` utility (`tools/yubikey_exec.py`)

```
py tools/yubikey_exec.py version                                   # no touch
py tools/yubikey_exec.py make-credential --out cred.json           # touch
py tools/yubikey_exec.py derive --in cred.json --ctx my-purpose    # no device
py tools/yubikey_exec.py run --ctx my-purpose --attest             # both, + the check
py tools/yubikey_exec.py sign --in cred.json --message "text"      # derive + sign + verify
py tools/yubikey_exec.py verify --in cred.json --ikm <hex> \
    --message "text" --signature <hex>                             # no device
```

- `version` prints firmware / AAGUID / CTAP versions / `previewSign` / `meets_arkg_firmware`.
- `make-credential --out FILE` writes the result as JSON (`save_result`) so `derive` can run **later, in another process, with the key unplugged** — that is the point of splitting them. The file holds no private key material (the ARKG private halves never leave the device) but it does identify the credential.
- `derive` is the offline half: `--ctx` labels the purpose, `--ikm <hex>` reproduces a specific derivation (otherwise 32 random bytes). It prints the derived public key (x/y + COSE CBOR) and the `arkg_args` (alg / key-handle blob / ctx + CBOR).
- `sign` derives (same `--ctx`/`--ikm` flags) then signs with the derived key and **verifies immediately** — a signature the derived public key cannot check is useless, and this is the only moment both halves are in hand. What gets signed is a mutually exclusive, required pair: `--message STRING` (UTF-8, hashed here) or `--digest <hex>` (a ready-made 32-byte SHA-256). Prints the signature hex; **note the `ikm` it prints** — you need it to verify later.
- `verify` is fully offline: it re-derives the public key from the saved credential plus `--ctx`/`--ikm` and checks `--signature <hex>`. `--ikm` is **required** here (a random one would produce a different key), and the error says so.
- `run` is `make-credential` followed by `derive`, plus signing when `--message`/`--digest` is given (optional there, unlike on `sign`).
- Exit code `3` = the signature did not verify. That is distinct from `1` (error) so a script can tell "the run broke" from "the key produced a bad signature".
- `--sig-out FILE` (on `sign`) writes the signature as one bare hex line, so a shell script can hand it to a later `verify` with a single `for /f`. Deliberately not JSON.
- `scripts\yubikey-arkg.cmd` wraps all five steps as a single command file for hardware runs, resolving the venv interpreter itself — it is the last section of this file.
- **The YubiKey check is optional and off by default** on every subcommand that could do it: `--attest` runs and reports it, `--require-yubikey` additionally turns a negative verdict into exit 2. `--roots PEM` overrides the trust anchors, `--intermediates PEM` overrides the bridging CAs (default: the bundled Yubico intermediates — needed for pinning to succeed at all, see §8.2), `--no-require-root` drops chain pinning (dev keys), `--seed-attestation` checks the seed key's own attestation instead of the credential's (on 5.8.0 firmware that is `fmt: none` and always fails — see §8.2). On `derive`, the check is silently skipped when the saved credential carries no attestation object.
- Exit codes: `0` ok, `1` error (no key / no `fido2` / bad input — including a `--ikm` shorter than 32 bytes), `2` "not a YubiKey" while `--require-yubikey`, `3` signature did not verify.


## The scripts that wrap them

How to run them is in [`scripts/README.md`](../scripts/README.md); below is why
they look like this.

### `scripts/test-request.cmd`

A launcher for `tools/test_request.py`: one permission request, one printed answer, with the hook's own verdict on it. Every option is forwarded, so `--help` there is authoritative. Deliberately thin — cmd cannot speak NATS, and `python -c "..."` mangles quotes here (the note in `yubikey-arkg.cmd`). It resolves the venv interpreter by path so it works from any console, needs **no elevation** (no FIDO device involved), and adds a one-line hint on failure: exit `1` says check that a responder and NATS are up, exit `3` says a responder answered but its key is not in this `--config`'s allowlist. CRLF and block-free, same reason as the other scripts.

**The examples, the option table, a real run's output and the `--input` quoting trap live in [`scripts/README.md`](../scripts/README.md)** — this section used to carry a second copy of all four, in a file whose own heading two lines up says how-to-run belongs there. One of them is worth naming even here, because it is a design decision rather than usage: `--input` needs *opposite* quoting in PowerShell and `cmd.exe`, each shell's working form being the other's argparse error, and that is why the script forwards arguments untouched instead of trying to normalise them.

### `scripts/yubikey-arkg.cmd`

A command-file run of the §8 hardware flow in **five steps**: `version` → `make-credential --out` (**first touch**) → `derive` → `sign --sig-out` (**second touch**) → `verify` (offline, no device). Not a self-checking test (a human finger is in the loop) — it drives the real `tools/yubikey_exec.py` and reports what the key returned. Args: `[--ctx LABEL] [--out FILE] [--attest] [--message STRING] [--ikm HEX] [--no-sign]`; `--no-sign` stops after step 3 (one touch, prints `[n/3]`).

- **Steps 3–5 must address the same derived key**, so the script mints **one** ikm up front (`os.urandom(32)`, via the venv python) and passes `--ikm` to all three. Without that, `derive` and `sign` would each roll their own ikm and produce unrelated keys.
- The ikm and the signature both travel **through files**, read back with `for /f`. Capturing them with `for /f "usebackq"` on a backquoted command does not work: cmd mangles the nested quotes and silently truncates `-c "import os;..."` at the first inner quote.
- Step 4 verifies inline (the tool always does, while both halves are in hand); step 5 is the meaningful one — it re-derives the public key from the credential file alone, with no key attached.
- **Must be launched from an already-elevated console** (`sudo cmd`, then the script) — it does not elevate itself; and because elevation drops `VIRTUAL_ENV` it calls `.venv\Scripts\python.exe` **by path** rather than trusting the `py` launcher, falling back to `py` only if the venv is missing.
- The credential and signature files are kept (default in `%TEMP%`, paths printed at the end) so `derive`/`verify` can re-run later with the key unplugged. Exit 0 = all steps OK, 1 = a step failed (each failure prints its specific likely cause), 3 = signed but the signature does not verify — distinguished from a missed touch on purpose. Run: `scripts\yubikey-arkg.cmd`.
