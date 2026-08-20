# tasks.md — what the documentation and the code disagree about, and what is unfinished

A consistency pass over every `README.md`, `CLAUDE.md` and subject document in the
repository, checked against the code they describe. Written 2026-08-20 against
`3506b27`.

Two things this file is **not**: a list of decisions (those live in the section
that owns them), and a second `approver-esp32/status.md` (that file is row-by-row
about the firmware and stays authoritative for it). This is the cross-repository
list: where a document has gone stale against its own code, and what every
document says is still owed, gathered in one place.

**Six defects have already been fixed and are gone from §1** — the README's
unusable hook entry and its silence about three of the four components, the two
documents that said `.claude/settings.json` wires only the status line,
`nats/CLAUDE.md`'s missing `activity` subject, `firmware.md` §10.15's claim that
a flash dump cannot yield the signing key, and `web.md`'s "nothing writes a
setting" in a file documenting the write path. What is left below is open.

## What is verified green

Run before the list below was written, and again after the six fixes, so a
defect is never confused with a broken build:

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

Ordered by how misleading each one is to somebody acting on it. Line numbers are
as of the fixes above.

### 1.1 `firmware.md` §10.15's `config set` field list is wrong

`firmware.md:948` lists the settable fields as `volume`, `brightness`, `dim`,
**`blank`**, `nats`, `tz`, `sntp`, `sync`, `wifi`.

`components/cli/console.cpp:798-808` has `volume`, `brightness`, `dim`,
**`dimlevel`**, **`sleep`**, `sync` (plus the string fields `nats`, `sntp`, and
`wifi`). There is no `blank` — it was renamed when §10.8.1's idle timer replaced
`dimSeconds`/`blankSeconds`, and `dimlevel` was never added to this list.

`commands.md:588-597` is correct. `firmware.md` is the one that drifted.

### 1.2 `firmware.md` §10.15 points at two settings rows that do not exist

- `firmware.md:757` — "wiping both is `Factory reset` (§10.8.5) — **two screen
  entries**, both two-step".
- `firmware.md:786` — "**The same restore is reachable from Settings**
  (§10.8.5), where there is a screen to confirm on."

`screens.md` §10.8.5's shipped list is seven rows: Wi-Fi, Status, Touch test,
Config save, Config reload, Reboot, Power off. There is no restore row and no
factory reset row — that section explicitly records them being dropped ("Bus,
display, time, key and registration, restore, factory reset — all of them are
`config set`, `nats url`, `date`, `keys` and `config restore` on the console
today").

### 1.3 Stale "not built yet" notes in `screens.md`

Three passages describe a state the firmware left behind, which makes them read as
open work when they are not:

- `screens.md:422` — "the navigator (`ui/navigator.h`) is **not wired to
  anything**, because four of the five screens it can name do not exist". It is
  wired (`screens.cpp:737`, `g_nav.Navigate(nav)`), and all seven screens exist.
- `screens.md:105` — "**Shared across all five:**". Seven.
- `screens.md:1563` — "The fallback access point is the other one, **if serving a
  form from it is ever worth it**: `esp_http_server` is in-tree, so it would cost
  no dependency". It was worth it and it is built — that is the whole of §10.16.

The first of those hides a real gap, split out as §2.10 below.

### 1.4 The §10.8 screen table still routes the limits screen by swipe

`screens.md`'s §10.8 table gives the Limits screen "Reached by: swipe left/right
from the clock", and §10.8.3 immediately says "It **arrives** rather than being
navigated to … The table above still lists it under 'swipe left/right from the
clock'". Self-acknowledged, but the table is the thing a reader scans first.

### 1.5 `tests.md` owes a test that is already in its own table

`tests.md:499` — "What is still owed is **the limits screen's arithmetic**:
`countdown()` against `render.rs`'s cases, the staleness threshold, and the §9.2
gauge scales".

All of it is in the table twelve rows above, and in
`host_test/test_limits.cpp` (21 tests), including the assertion that the two
scales cannot drift into each other.

### 1.6 `build.md` §10.4's dependency table is broken markdown

`approver-esp32/build.md`: the "New dependencies, all four approved" table opens
at line 88 with two rows (LVGL, the panel/touch drivers), then 40 lines of prose
run from line 95, and then lines **136–137** are two more table rows — libsodium
and the NATS client — orphaned in the middle of the text. They render as stray
pipe-delimited prose, and the sentence "all four approved" points at a table that
visibly holds two.

Two smaller drifts in the same file:

- `build.md:221` — "'LVGL v9' is the approval, **`9.3.0`** is the record".
  Resolved to **9.4.0** (`dependencies.lock`), which the same file says three
  paragraphs earlier.
- `build.md:401` — "the **five** screens of §10.8". Seven.

### 1.7 `statusline/CLAUDE.md` §9.4 says six modules, lists seven

`statusline/CLAUDE.md:111` — "`src/lib.rs` — the library root; the **six** modules
below." `src/lib.rs` declares seven: `activity`, `config`, `json`, `link`, `nats`,
`render`, `status` — and the bullet list under that line has all seven.

### 1.8 `tests/CLAUDE.md`'s file table is missing a test file

`tests/test_esp32_web_pages.py` exists, is documented in
`approver-esp32/tests.md` §10.11 tier 2, and is not in `tests/CLAUDE.md`'s "The
files" table.

### 1.9 Two `nats/` config comments cite a section that moved

`nats/docker-compose.yml:18` and `nats/nats-server.conf:10` both cite
`approver-esp32/CLAUDE.md` §10.5. §10.5 now lives in
`approver-esp32/protocol.md` — `nats/CLAUDE.md` itself already links there
correctly. Section numbers are stable by design, so the number is right and the
filename is not.

### 1.10 A Cyrillic typo in a sentence about the trust boundary

`approver-esp32/CLAUDE.md:204` — "it was bound to **localhosлоt**". Two Cyrillic
characters in the middle of the word, in the paragraph that explains why the bus
stopped being a localhost bus.

### 1.11 Root `CLAUDE.md` §1's ESP-IDF paragraph is a release behind §2's

`CLAUDE.md:16` opens "**C++ / ESP-IDF (`approver-esp32/` only, §10) — approved;
the library layer is written, the protocol is not**", and later says "**two of
§10.8's five screens are on the glass**: the clock face … and the request card".

The protocol is written (§10.2, §10.6, §10.7 are all on the board) and all seven
screens exist. Root §2's own `approver-esp32/` row is current — the two halves of
the same file disagree.

### 1.12 Counted things that have drifted

Not wrong in substance, but each is a number a reader would take literally:

| Claim | Where | Actual |
|---|---|---|
| "the **92** source files that carry a `CLAUDE.md §10.8.2`" | `approver-esp32/CLAUDE.md:26` | 104 files under `components/`, `main/`, `host_test/` |
| `web_paths` "host-tested (**11** tests)" | `web.md:569` | 28 (`test_web_paths.cpp`), plus 29 in `test_web_settings.cpp` |
| web "**52** host tests over what a URL may read and what a form may write" | `status.md` | 57 |
| the idle timer's "**25** host tests" | `status.md` | 30 (`test_idle.cpp`) |
| the Wi-Fi screen's "**42** tests" | `status.md`, `screens.md` | 40 (`test_wifi_view.cpp`) |
| registration's "**25** host tests" | `status.md` | 24 (`test_registration.cpp`) |
| the signing bytes' "**14** tests" | `status.md` | 11 (`test_signing.cpp`) + 8 (`test_vectors.cpp`) |

The suite total (690) and every other per-file count I checked are correct.

### 1.13 Root `CLAUDE.md` §2's project-file list omits two tracked files

`.mcp.json` (the LVGL preview server, referenced from `build.md` §10.12.1) and
`LICENSE` are both committed and neither is in the "Project-level files" list.
Also: `screens/7. web-approver screenshot.png` is tracked, but that row describes
`screens/` as "the screenshots `yubikey-hackaton.md` walks through" and that file
walks through 1–6 only.

---

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
- **`screens/7. web-approver screenshot.png`** is committed and referenced by
  nothing (see §1.13).
- **The `soon` rows in §10.8.5's settings list** are the owner's placeholder
  ("пока непонятно, потом чтото добавим") and deliberately not drawn. Listed here
  only so it is not rediscovered as a gap.
