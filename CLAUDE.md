# Claude permission approver

The applied goal of the project is to move Claude Code's permission confirmation outside the terminal. Instead of the interactive permission prompt, the `PermissionRequest` hook sends a request into NATS (request-reply), an external human responder signs the decision with an Ed25519 key, and the hook verifies the signature and hands Claude Code an `allow`/`deny` verdict. Trusted responder keys are provisioned through a separate registration process using one-time tokens. The full protocol, message contracts, and fail-safe requirements live in [`approver/CLAUDE.md`](approver/CLAUDE.md) (§6–§7).

It runs on a local [NATS](https://nats.io/) sandbox on Docker Desktop under Windows: the whole infrastructure comes up with a single `docker compose` — see [`nats/CLAUDE.md`](nats/CLAUDE.md) (§3–§4).

## 1. Repository rules

- **TDD.** Test first, then code. The red → green → refactor cycle: write a failing test for the behavior → the minimal code to make it green → refactor under green tests. New functionality and bugfixes come together with tests; a PR without tests for the changed behavior is not merged. The test runner is **pytest**, tests live in `tests/`, files `test_*.py`.
- **List of libraries in use** (the only approved ones):
  - **Runtime:** `nats-py` (NATS client), `cryptography` (Ed25519 / ECDSA P-256 — sign/verify).
  - **Optional runtime:** `fido2` (YubiKey / CTAP2 / WebAuthn — the ARKG `previewSign` flow of §8). **Optional on purpose:** only `lib/yubikey.py`, `tools/yubikey_exec.py` and `approver/responder_yubikey.py` (§8.7) may need it — the **core approval path** (`hook.py` / `responder.py` / `registration_handler.py` / `protocol.py`) must never import it, so the §6/§7 flow keeps working on a bare `uv sync`. Declared in the `yubikey` extra, not in `[project.dependencies]`.
  - **Dev/tests:** `pytest`.
  - The Python standard library — no restrictions.
  - **Rust (`statusline/` only, §9.4).** `async-nats` (the official NATS client — the Rust counterpart of `nats-py`), `tokio` (`rt`/`time`/`macros` — `async-nats` is async and ships no runtime), `serde` + `serde_json` (the wire format, already inside `async-nats`'s tree), and `futures` **dev-only** (reading a subscription in the integration test). Declared in `statusline/Cargo.toml`, locked in `statusline/Cargo.lock`; the Rust standard library has no restrictions. None of this reaches the Python side — `uv sync` is unaffected.
  - **C++ / ESP-IDF (`approver-esp32/` only, §10) — approved, not yet written.** The firmware is **C++**, with C only where a C ABI is forced, and **no dynamic memory** beyond one named exception (`std::string`) — everything is allocated statically and lives for the life of the device (§10.14.1); it is built library layer first, logic second (§10.14.2). The folder still holds documentation rather than firmware, but the dependency set is signed off: **ESP-IDF v5.5.2** and its in-tree components (`esp_wifi`/`lwip`, `mbedtls`, cJSON, `nvs_flash`, `esp_hmac`/`efuse`, `esp_console`, `esp_lcd`), **LVGL v9 + `espressif/esp_lvgl_port`**, the **CO5300** panel and **CST9220** touch drivers, and **libsodium** for Ed25519 — mbedTLS has no EdDSA and §6's server key is Ed25519 by fixed protocol. The bus client is **`debsahu/espidf-nats`** (registry, `^1.4.0`, MIT, header-only C++): it covers the §10.5 subset, supports the queue group §6 needs and the reply-to subject §7 answers into, and brings the TLS §10.3 will want. Each choice is argued in [`approver-esp32/CLAUDE.md`](approver-esp32/CLAUDE.md) **§10.4**; versions are declared in `main/idf_component.yml` and locked in `dependencies.lock` (committed), the way `uv.lock` and `Cargo.lock` pin the rest. On the host side only, `lvgl-mcp-server` renders the screens without a board (**§10.12.1**) — a design tool, linked into nothing. None of this reaches the Python, Rust or Node halves.
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
| `approver-web/` | a third client: the same responder as a web page (Next.js + Chakra UI — the Node half of the repo) | [`approver-web/CLAUDE.md`](approver-web/CLAUDE.md) |
| `approver-esp32/` | a fourth client, **planned**: the responder as firmware — ESP32-C6-Touch-AMOLED-2.16 on ESP-IDF, one press on a screen on the desk | [`approver-esp32/CLAUDE.md`](approver-esp32/CLAUDE.md) — **§10** the firmware |
| `lib/` | shared modules — `bus.py`, `config.py`, `crypto.py`, `yubikey.py`, and the pinned Yubico PEM bundles | [`lib/CLAUDE.md`](lib/CLAUDE.md) — **§8**–**§8.4**, **§8.6** (YubiKey / ARKG) |
| `tools/` | command-line utilities — `test_request.py` (probe a responder), `yubikey_exec.py` (the ARKG CLI) | [`tools/CLAUDE.md`](tools/CLAUDE.md) — **§8.5** |
| `scripts/` | Windows command files that drive the flows end to end | [`scripts/README.md`](scripts/README.md) — how to run each one; the rationale sits with the flow each drives (`e2e-registration` / `e2e-approval` / `yubikey-approval` in `approver/`, `test-request` / `yubikey-arkg` in `tools/`) |
| `statusline/` | the Claude Code status line in Rust — the model plus how much of the 5h / 7d rate limits is spent, on screen and published to NATS | [`statusline/CLAUDE.md`](statusline/CLAUDE.md) — **§9** the line, **§9.7** the `status` subject, **§9.8** the connection dot |
| `nats/` | docker-compose: the NATS server, dashboard and `nats-box` (CLI) | [`nats/CLAUDE.md`](nats/CLAUDE.md) — **§3** the compose file, **§4** NATS concepts the rest of the repo relies on |
| `tests/` | the pytest suite (`test_*.py`) and the `conftest.py` markers that keep it green with no Docker and no hardware | [`tests/CLAUDE.md`](tests/CLAUDE.md) |
| `screens/` | the screenshots `README.md` walks through — no docs of their own | — |

`lib/`, `approver/` and `tools/` have an `__init__.py` so `import lib.bus` /
`from approver import protocol` resolve; the scripts additionally prepend the
repo root to `sys.path` so they also work when run directly by path.

Project-level files:

- `README.md` — the short form: the problem, the solution, and the one command that runs the whole loop. `full-readme.md` — the long form: setup, the smoke tests, the command reference. Both are for a reader arriving at the repository; the `CLAUDE.md` files are for working inside it, and are the ones to keep authoritative.
- `pyproject.toml` — project metadata and dependencies (runtime + dev group); the source of truth for dependencies. In `[tool.pytest.ini_options]`: `pythonpath=["."]` (importing `lib.*` in a non-package project), `testpaths=["tests"]`, `--basetemp=.pytest_tmp` (the default temp root is unavailable in this sandbox).
- `uv.lock` — locked versions (uv), committed to the repository.
- `statusline-config.example.json` — the defaults for the status line's runtime config (§9.9). The live copy lives next to the built binary, not here; this is the committed record of the format, and a test asserts it matches the defaults.
- `.claude/settings.json` — committed, project-wide: it wires the status line to the built binary (§9.5). `.claude/settings.local.json` is the per-machine half — granted permissions and the `PermissionRequest` hook with this machine's absolute interpreter path (§7) — and is git-ignored.
- `.gitignore` — `__pycache__/` + `*.py[cod]`, `.venv/`, `.pytest_cache/`, `.pytest_tmp/`, `.idea/`, `statusline/target/` + `statusline-config.json` (§9.9), `.claude/settings.local.json`, the secret-bearing runtime configs `approver/responder-config.json` / `approver/handler-config.json` / `approver/responder-yubikey-config.json` (their `*.example.json` siblings carry no secrets and **are** committed), and saved `yubikey-exec` credentials (`/cred.json`, `*-cred.json` — the filenames the §8.5 examples use). `approver-web/` keeps its own `.gitignore` for the Node half (`node_modules/`, `.next/`, its `config.json`, and `.claude/skills/` — restored from the committed `skills-lock.json`).

> §3 (the compose file) and §4 (NATS concepts) moved to
> [`nats/CLAUDE.md`](nats/CLAUDE.md), keeping their numbers — `tests/conftest.py`
> and `statusline/src/nats.rs` cite them.

## 5. Python (host)
- Run via the **`py`** launcher (Python 3.14.6): `py script.py`, `py -m pytest`, `py -c "..."`.
- The real interpreter that `py` points to: `C:\Users\User\AppData\Local\Python\pythoncore-3.14-64\python.exe`.
  `C:\...\WindowsApps\python.exe` is the Microsoft Store stub, do NOT use it.
