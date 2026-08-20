# tasks.md — what the documentation and the code disagree about, and what is unfinished

A consistency pass over every `README.md`, `CLAUDE.md` and subject document in the
repository, checked against the code they describe. Written 2026-08-20 against
`3506b27`.

Two things this file is **not**: a list of decisions (those live in the section
that owns them), and a second `approver-esp32/status.md` (that file is row-by-row
about the firmware and stays authoritative for it). This is the cross-repository
list: where a document has gone stale against its own code, and what every
document says is still owed, gathered in one place.

**Every documentation defect this pass found has been fixed** — §1 is a summary
of what they were, not a to-do list. What is open is §2: the work the documents
themselves say is unfinished.

## What is verified green

Run before the list below was written, and again after every fix, so a defect is
never confused with a broken build:

| Tier | Command | Result |
|------|---------|--------|
| Python | `.venv\Scripts\python.exe -m pytest -q` | **446 passed, 24 skipped** (no NATS, no YubiKey, no board) |
| Rust | `cargo test -q` (in `statusline/`) | **58 passed** across four binaries |
| Rust format | `cargo fmt --check` | clean |
| Node | `npm test` (in `approver-web/`) | **28 passed** |
| Host tier (ESP32) | `RUN_TEST` count in `approver-esp32/host_test/` | **690**, matching `tests.md` §10.11 |
| Markdown links | every relative link in every tracked `.md` | all resolve |

---

## 1. Documentation that contradicts the code

**Empty — all nineteen are fixed.** This section held the places where a document
had gone stale against its own code; every one of them has been corrected in the
document that owned it, and the findings are in the git history rather than kept
here as a list of things already done.

What they were, in one line each, so a reader knows what was swept and can go
looking if a fix reads oddly: the README's unusable hook entry and its silence
about three of the four components; two documents claiming `.claude/settings.json`
wires only the status line; `nats/CLAUDE.md`'s bus table missing `activity`;
`firmware.md` §10.15 claiming a flash dump cannot yield the signing key, and its
`config set` list naming a field that no longer exists, and two settings rows that
were removed; three "not built yet" notes in `screens.md` that the firmware had
outgrown, and its §10.8 table still routing the limits screen by swipe; a test
`tests.md` said was owed and had already written; a broken markdown table in
`build.md` §10.4 plus a stale LVGL version and screen count;
`statusline/CLAUDE.md` counting six modules and listing seven; a test file missing
from `tests/CLAUDE.md`; two `nats/` config comments citing a file the section
moved out of; a Cyrillic typo inside "localhost"; root `CLAUDE.md` §1 a release
behind its own §2; seven drifted test counts; and two tracked files absent from
root §2's project-file list.

**The pass that produced them is worth repeating rather than the list.** Three
kinds of drift accounted for nearly all of it, and none is caught by any test:
a count written as a number (screens, modules, test cases, source files), a
"not done yet" note left behind by the thing getting done, and a section that
moved between files while its citations stayed put. A `§10.5` in a comment names
a section, not a file, and that is exactly what let two of them rot quietly.

## 2. Unfinished work

Everything below is something a document says is not done, or a gap the check
found. Nothing here is a new proposal.

### 2.1 Key custody on the ESP32 shipped as its fallback (§10.6, §10.15, §10.12)

The largest open item. Its cost is now stated correctly in the docs — what is
unfinished is the operation itself.

- No eFuse key is burned, so the Ed25519 seed is **32 unencrypted bytes in NVS**
  (`approver` namespace) and `esptool read_flash` gives up the signing key.
- NVS encryption needs flash encryption, which needs the same one-way eFuse
  operation. `nvs_keys` stays reserved and empty.
- Both routes are written — `components/crypto/device_key.h` tries the fuse first
  at every boot — so **burning a key needs no new firmware**, and the firmware
  deletes the stale seed and reports a new identity when it happens.
- Deliberate, at the repository owner's decision (one board, one-way operation).
  What is owed is not code; it is the decision, taken with a development board
  that has not been through it (`build.md` §10.12).
- The same decision settles the `storage` partition question (`firmware.md`
  §10.15: SPIFFS cannot be encrypted at rest; FATFS can, and the JSON would be
  untouched) — worth taking together rather than separately.

### 2.2 The on-screen keyboard (§10.8.6)

Designed in millimetres — a 6×5 grid of 6 mm keys, alphabetical, with a preview
bubble, 70 px of header and 410 of keyboard — and not built. Consequences:

- a Wi-Fi **password** still arrives over the console (`wifi join`) or over
  §10.16's web form; the Wi-Fi screen reads a password and never types one;
- there is no way to **add** a network from the glass (the owner's instruction —
  the scan row refuses with a sentence when there is no record to fill).

The two costs it would have are already paid: Montserrat 28 is enabled and
referenced since §10.8.3, and a thirty-key `lv_buttonmatrix` is a few hundred
bytes of LVGL's pool.

### 2.3 `approver-web`'s own "What is still missing"

From `approver-web/CLAUDE.md`:

1. **Tests are not committed.** The two cross-language checks were run as scratch
   scripts: a browser signature accepted by `hook.verify_reply`, and the
   non-extractable key surviving a browser restart. Both need `agent-browser`, so
   they would be an opt-in tier like §8.6's touch tests — not part of a bare
   `py -m pytest`. The repo's TDD rule (root §1) wants them in `tests/`.
2. **No `.cmd` script** in `scripts/` for the register → request → signed-decision
   loop, in the style of `e2e-approval.cmd`. Confirmed absent: `scripts/` has six
   files and none of them is the web one. `scripts/README.md`'s table has no row
   for it either.
3. **Multiple browsers** share one `key_id`, so a second browser registering
   rotates the first out. Fine for one operator; distinct `key_id`s and tokens
   would work today if it is ever wanted.
4. **No authentication on the page.** Anyone who can open it in that browser
   profile can approve a `rm -rf`; anyone who can reach the port can hand
   `POST /api/register` a token. Acceptable on loopback, "unacceptable the moment
   it binds to anything but the loopback interface".

### 2.4 The ESP32 web server's write path has no authentication decision (§10.16, §10.3)

The write path shipped ahead of the question `web.md` said it would need first.
What is actually open:

- **authentication, or binding to the access-point interface only.** The server
  exists to configure a device that has no network yet; on the LAN §10.3 already
  put the boundary at the router *for reading*, and a writable config moves that
  boundary. `web.write: false` is the only switch today and is explicitly not
  authentication.
- **the Wi-Fi screen has no row saying whether the page is up** — `web.md:544`
  says "it should". The system status page has the row; §10.8.6's screen does
  not.

### 2.5 TLS and credentials on the bus (§10.3, §3/§4, root §7)

The bus is unauthenticated, every subject is open, and the client port is
published on **every interface** so the ESP32 can reach it over Wi-Fi. Three
things `nats/CLAUDE.md` names as holding that decision up, of which two are
recommendations rather than facts about the current compose file:

| Condition | State |
|---|---|
| nothing port-forwarded to the host | not verifiable from the repository — "worth verifying once, deliberately" |
| `8222` and `8080` bound back to `127.0.0.1` | **not done** — `docker-compose.yml` publishes both on every interface |
| `4222` published on the LAN address rather than every interface | **not done** — `"4222:4222"`; a VPN or overlay joined later carries this bus onto it silently |

TLS + credentials stay the real fix. The device side is now configuration rather
than work (`debsahu/espidf-nats` speaks TLS 1.2/1.3 with server-cert validation),
so this is a `nats/` change, not a firmware one.

### 2.6 ESP32 device-tier checks written but not performed

Each of these is a thing no host test can reach, listed with what it needs:

| What | Needs | Where it is recorded |
|---|---|---|
| the **pressed half** of `tests/test_esp32_device.py` — one press producing a reply `hook.verify_reply` calls trusted, and the nine tamper assertions that come free with it | a finger | `tests.md` tier 3, `status.md` |
| the settings list's **scroll by finger**, a row pressed, and the three seconds of `saved` | a finger (`screen` on the console deliberately cannot press a row or draw a gesture) | `screens.md` §10.8.5 |
| the touch calibration's **on-glass half** — four crosses, the fit applied | a finger | `status.md` |
| `poweroff` actually switching the board off | the cable out, and with the cable out there is no console to watch it from — waits for a battery-powered session | `protocol.md` §10.7, `screens.md` §10.8.5 |
| Wi-Fi **auth-failure classification** (sticky, and spelled differently from "no such network") | a network whose password is deliberately wrong | `firmware.md` §10.9 |
| SNTP's **six-hour interval** and its backoff | a device up that long | `status.md` |

### 2.7 Frame-level fuzzing of the NATS client is still owed (§10.5)

The size half has been fired at the real server and found something worth having
(oversized messages drop the socket; bounded at the server with
`--max_payload=65536`). What has **not** been fired:

- a `MSG` length that lies about the bytes that follow;
- a truncated header;
- a server that stops mid-payload.

All three need something pretending to be a NATS server rather than a real one.
The frame parser is `debsahu/espidf-nats`'s now, not ours, which is exactly why it
has to be attacked rather than trusted.

### 2.8 The responder is anonymous on the bus (§10.5)

`debsahu/espidf-nats` builds its own `CONNECT` and sends no `name` field, so the
device shows up in `/connz` as `name: null, lang: "espidf"`. §10.2 gives it a
`key_id` precisely so it can be told apart, and `/connz` is where an operator
looks when two clients are on one subject and only one is answering — which is
§6's "Multiple clients", and will bite the first time the YubiKey responder and
the device are both up.

Not fixable from here: `NATS_CLIENT_LANG`/`NATS_CLIENT_VERSION` are unguarded
`#define`s and there is no name field in the `CONNECT` builder. It is an upstream
patch or a vendored fork. Recorded rather than fixed, with the IP as the fallback
identifier.

### 2.9 The worst-case heap number has not been measured (§10.14.1, §10.5)

`firmware.md` §10.14.1 names the measurement it wants and it has not been taken:
**a request arriving during a Wi-Fi scan with the codec running**, read as
`heap_caps_get_minimum_free_size()`. The numbers that exist are each from one
condition at a time; the one that says whether the device is safe is the
combination. §10.5 makes the same argument from the other end after a 64 KB
message took the low-water mark to 23,068 free.

### 2.10 The clock screen is missing two things §10.8.2 specifies

§10.8.2's list for the home screen is "the time, the date, the link indicator,
**`key_id` when registered**, and **a gear**". Neither is drawn —
`components/screens/clock_screen.cpp` has no reference to either. The reason
`screens.md:422` gives ("there is no registration to name and nothing to open") is
no longer true: registration works and settings is a swipe up.

Low stakes — settings is reachable three other ways — but the section still
promises them.

### 2.11 The two shipped config files disagree about the AP password (§10.9)

`spiffs_image/config.json` raises a **WPA2** access point;
`spiffs_image/config.init.json` leaves it **open** — so a restore (or holding
`KEY` at boot) opens the device's own access point. Deliberate when the AP served
nothing and stayed up two minutes at a time. §10.16 now serves a *writable*
configuration site on that AP, and §10.9 named this as the line to revisit "when
§10.8.6 gives it a screen to serve — and the two files should stop disagreeing at
the same time".

### 2.12 Battery and sleep (§10.13)

Explicitly deferred: light sleep with a socket open, or waking on Wi-Fi, changes
the "is it connected" story of §10.8.1. The collision is already named — a screen
that blanks is fine, a *radio* that sleeps means requests arrive late or not at
all, and a responder that is asleep is a responder that times out.

### 2.13 There is no CI

No `.github/`, no workflow, nothing that runs any of the four test tiers on a
push. Root §1's TDD rule is enforced by hand today. `firmware.md` §10.14.4 already
flags the shape of the ESP-IDF half of it ("Their `sdkconfig.ci` — a second
configuration kept for CI — is worth remembering when there is CI").

Four commands would cover it, and three of them need nothing but a checkout:
`py -m pytest -q`, `cargo test` + `cargo fmt --check`, `npm test`, and
`approver-esp32/host_test/run.cmd` (which needs `managed_components/`, so one
`idf.py build` first).

### 2.14 Smaller open ends

- **`tools/make_vectors.py` has no repo-level runner.** The staleness guard
  (`tests/test_esp32_vectors.py`) is in the pytest suite and passing, so this is
  cosmetic — but regenerating is a hand-typed command under the venv.
- **The ESP32 host suite is not reachable from `scripts/`.** Every other tier has
  either a documented one-liner in a `CLAUDE.md` or a `.cmd`; this one is
  `approver-esp32/host_test/run.cmd`, which `scripts/README.md` does not list.
- **`screens/7. web-approver screenshot.png`** is committed and referenced by no
  document. Root §2 now says so rather than implying `yubikey-hackaton.md` walks
  through it, so what is left is the choice: give it a place in that walkthrough,
  or drop it.
- **The `soon` rows in §10.8.5's settings list** are the owner's placeholder
  ("пока непонятно, потом чтото добавим") and deliberately not drawn. Listed here
  only so it is not rediscovered as a gap.
