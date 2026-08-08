# tests/ — the pytest suite

The repository is TDD ([`../CLAUDE.md`](../CLAUDE.md) §1): a behaviour change
arrives with the test for it. This is the Python suite — files `test_*.py`, run
with `py -m pytest` from the repository root. The Rust status line has its own
tier (`cargo test`, §9.6 in [`../statusline/CLAUDE.md`](../statusline/CLAUDE.md))
and `approver-web` its own (`npm test`, `node:test` — no runner dependency).

**A bare `py -m pytest` on a fresh clone is green.** Nothing here requires
Docker, a YubiKey, or the `fido2` extra; everything that could is guarded and
skips with a reason instead of failing. That invariant is the point of the file
layout below — do not add a test that fails on a machine without hardware.

The runner is configured in `pyproject.toml` under `[tool.pytest.ini_options]`:
`pythonpath=["."]` (this is a non-package project, so the repo root goes on
`sys.path` for `import lib.*` / `from tests.conftest import …`),
`testpaths=["tests"]`, and `--basetemp=.pytest_tmp` because the default temp root
is not writable in this sandbox.

## `conftest.py` — what makes the suite skippable

- `requires_nats` — probes `127.0.0.1:4222` once at collection (§3) and skips the
  integration tests when the broker is down.
- `requires_yubikey` / `requires_preview_sign` / `requires_yubikey_touch` — the
  three hardware tiers of §8.6. `_probe_yubikey()` runs once, needs no PIN and no
  touch, and turns **any** failure into a skip reason rather than a collection
  error. The touch tier is gated on `AI_REMOTE_YUBIKEY_TOUCH=1` on purpose: an
  unattended run must never block waiting for a finger.
- `run_async(coro)` — drives async bodies through `asyncio.run`. We do not add
  `pytest-asyncio`; it is not on the approved list (§1).

## The files

| File | Covers | Needs |
|------|--------|-------|
| `test_bus.py` | `lib/bus.py` — request-reply, queue groups, dropped non-JSON | a live NATS server |
| `test_config.py` | `lib/config.py` — versioning, atomic save, dict access | — |
| `test_crypto.py` | `lib/crypto.py` — both schemes, fail-safe `verify` | — |
| `test_protocol.py` | `approver/protocol.py` — canonicalization and both signing-bytes shapes (§6/§7) | — |
| `test_hook.py` | `approver/hook.py` — the fail-safe invariant: every bad/absent/mismatched reply falls through to the prompt | — |
| `test_responder.py` | `approver/responder.py` — pure halves; the register round trip is an integration test | partly NATS |
| `test_registration_handler.py` | `approver/registration_handler.py` — token minting, allowlist writes, the server key (§6) | partly NATS |
| `test_test_request.py` | `tools/test_request.py` — the pure halves only | — |
| `test_yubikey.py` | the half of `lib/yubikey.py` that needs no `fido2` — including that the module **imports cleanly with the extra absent** | — |
| `test_yubikey_fido2.py` | the rest of `lib/yubikey.py`, with fake hardware (§8.4) | `fido2` |
| `test_yubikey_exec.py` | `tools/yubikey_exec.py` (§8.5), `derive` end to end | `fido2` |
| `test_responder_yubikey.py` | `approver/responder_yubikey.py` (§8.7), same fakes | `fido2` |
| `test_yubikey_integration.py` | the real device (§8.6) — three tiers | a YubiKey |

Two conventions worth keeping:

- **The `fido2` files skip, they do not fail.** `pytest.importorskip("fido2", …)`
  at the top, imports below it marked `# noqa: E402`. That is what keeps the
  optional extra optional (§1).
- **Fake hardware is defined once.** `test_yubikey_fido2.py` owns `make_result`,
  `Chain` and `_der` — a synthetic ARKG seed key in the real COSE shape plus a
  throwaway attestation CA — and `test_responder_yubikey.py` imports them rather
  than growing a second copy. It stands in for the device with a P-256 pair whose
  private half the test holds, which is the same verification path (§8.4).

What genuinely cannot be faked here is a *positive* derived-key sign→verify round
trip: `fido2` exposes only `derive_public_key`, and the private half exists solely
inside the authenticator. That one lives in §8.6.
