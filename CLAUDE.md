# Claude permission approver

The applied goal of the project is to move Claude Code's permission confirmation outside the terminal. Instead of the interactive permission prompt, the `PermissionRequest` hook sends a request into NATS (request-reply), an external human responder signs the decision with an Ed25519 key, and the hook verifies the signature and hands Claude Code an `allow`/`deny` verdict. Trusted responder keys are provisioned through a separate registration process using one-time tokens. The full protocol, message contracts, and fail-safe requirements live in [`approver/CLAUDE.md`](approver/CLAUDE.md) (§6–§7).

A local sandbox for experimenting with [NATS](https://nats.io/) on Docker Desktop under Windows. The infrastructure comes up with a single `docker compose` (section 3): the NATS server itself with JetStream enabled, a web dashboard to observe the bus, and a `nats-box` container with the `nats` CLI for manual checks of publishes, streams, and subscriptions.

## 1. Repository rules

- **TDD.** Test first, then code. The red → green → refactor cycle: write a failing test for the behavior → the minimal code to make it green → refactor under green tests. New functionality and bugfixes come together with tests; a PR without tests for the changed behavior is not merged. The test runner is **pytest**, tests live in `tests/`, files `test_*.py`.
- **List of libraries in use** (the only approved ones):
  - **Runtime:** `nats-py` (NATS client), `cryptography` (Ed25519 / ECDSA P-256 — sign/verify).
  - **Optional runtime:** `fido2` (YubiKey / CTAP2 / WebAuthn — the ARKG `previewSign` flow of §8). **Optional on purpose:** only `lib/yubikey.py`, `tools/yubikey_exec.py` and `approver/responder_yubikey.py` (§8.7) may need it — the **core approval path** (`hook.py` / `responder.py` / `registration_handler.py` / `protocol.py`) must never import it, so the §6/§7 flow keeps working on a bare `uv sync`. Declared in the `yubikey` extra, not in `[project.dependencies]`.
  - **Dev/tests:** `pytest`.
  - The Python standard library — no restrictions.
  - **Where they are declared.** The single source of truth is `pyproject.toml`: runtime in `[project.dependencies]`, optional runtime in `[project.optional-dependencies].yubikey`, dev/tests in `[dependency-groups].dev` (PEP 735). Versions are locked in `uv.lock` (committed). The project is non-package (`[tool.uv] package = false`, no `[build-system]`): we do not build a distribution, there is no editable install.
  - **Installation (uv):** `uv sync` (runtime + dev), `uv sync --no-dev` (runtime only), `uv sync --extra yubikey` (adds `fido2`). No `requirements*.txt` — they do not exist.
  - **Optional deps must degrade, not crash.** A module backed by an extra has to import cleanly when the extra is absent (lazy/guarded import + an `X_AVAILABLE` flag + a `require_X()` raising an actionable error), and its tests must skip rather than fail. `lib/yubikey.py` is the reference implementation of this pattern.
- **You may not pull in new libraries without confirmation.** Any dependency outside the list above (including transitive ones that drag in noticeable weight, and dev tools) is added only after explicit sign-off from the repository owner. The preference is to solve the task with the standard library. If a new dependency really is needed — ask first, then add it and update this list.

## 2. Structure

**Every folder documents itself.** This is the map; the per-folder files own the
protocol and API detail. Section numbers are **global and stable** — code
docstrings cite them (`CLAUDE.md §6`, `§8.4`), so a section keeps its number
wherever it lives.

| Path | What is in it | Its own docs |
|------|---------------|--------------|
| `approver/` | the approval flow: `registration_handler.py` (**the server** — owns the allowlist) and two clients, `responder.py` (software key) and `responder_yubikey.py` (key on a YubiKey); plus `hook.py` (the Claude Code side) and `protocol.py` (the shared wire format) | [`approver/CLAUDE.md`](approver/CLAUDE.md) — **§6** registration, **§7** the hook protocol, **§8.7** the YubiKey responder |
| `approver-web/` | a third client: the same responder as a web page (Next.js + Chakra UI — the only non-Python part of the repo) | [`approver-web/CLAUDE.md`](approver-web/CLAUDE.md) |
| `lib/` | shared modules — `bus.py`, `config.py`, `crypto.py`, `yubikey.py`, and the pinned Yubico PEM bundles | [`lib/CLAUDE.md`](lib/CLAUDE.md) — **§8**–**§8.4**, **§8.6** (YubiKey / ARKG) |
| `tools/` | command-line utilities — `test_request.py` (probe a responder), `yubikey_exec.py` (the ARKG CLI) | [`tools/CLAUDE.md`](tools/CLAUDE.md) — **§8.5** |
| `scripts/` | Windows command files that drive the flows end to end | [`scripts/README.md`](scripts/README.md) — how to run each one; the rationale sits with the flow each drives (`e2e-registration` / `e2e-approval` / `yubikey-approval` in `approver/`, `test-request` / `yubikey-arkg` in `tools/`) |
| `nats/` | docker-compose: the NATS server, dashboard and `nats-box` (CLI) | §3 below |

`lib/` and `approver/` have an `__init__.py` so `import lib.bus` / `from approver
import protocol` resolve; the scripts additionally prepend the repo root to
`sys.path` so they also work when run directly by path.

Project-level files:

- `tests/` — pytest tests (`test_*.py`), see §1. `conftest.py`: the `requires_nats` marker (skips integration tests when NATS is unreachable) and `run_async()` (drives async bodies via `asyncio.run` — we do not add `pytest-asyncio`). `test_yubikey.py` holds the YubiKey tests that need nothing external; `test_yubikey_fido2.py` guards the rest with `pytest.importorskip("fido2")` and fakes the hardware (synthetic ARKG seed key + throwaway attestation CA), so no YubiKey is required to run the suite. `test_responder_yubikey.py` covers §8.7 the same way — it imports `make_result` / `Chain` from `test_yubikey_fido2.py` rather than duplicating them, and stands in for the device with a P-256 pair whose private half it holds (see §8.4).
- `pyproject.toml` — project metadata and dependencies (runtime + dev group); the source of truth for dependencies. In `[tool.pytest.ini_options]`: `pythonpath=["."]` (importing `lib.*` in a non-package project), `testpaths=["tests"]`, `--basetemp=.pytest_tmp` (the default temp root is unavailable in this sandbox).
- `uv.lock` — locked versions (uv), committed to the repository.
- `.gitignore` — `__pycache__/` + `*.py[cod]`, `.venv/`, `.pytest_cache/`, `.pytest_tmp/`, `.idea/`, the secret-bearing runtime configs `approver/responder-config.json` / `approver/handler-config.json` / `approver/responder-yubikey-config.json` (their `*.example.json` siblings carry no secrets and **are** committed), and saved `yubikey-exec` credentials (`/cred.json`, `*-cred.json` — the filenames the §8.5 examples use).

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
