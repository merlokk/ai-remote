# tasks.md — what is still open

The cross-repository list of what is **still open**: what every document says is
owed, gathered in one place and checked against the code it describes. Started
2026-08-20 against `3506b27`, and kept as a list of open work — an item is deleted
when it is done, not ticked off, because the reason it was done belongs in the
document that owns the subject and the history belongs in git.

**So the numbering has gaps, and they are not a mistake to tidy up.** Other files
cite these numbers (`web.md` names §2.5, `protocol.md` names §2.7), so a number
belongs to the item that was given it — renumbering after a deletion would silently
repoint every citation. Three gaps so far: **§2.9**, the clock screen's two unbuilt
promises, struck from §10.8.2 rather than built; **§2.10**, the two shipped config
files disagreeing about the access point's key, which they no longer do; and
**§2.12**, three smaller loose ends — a runner for the parity vectors, a way into
the ESP32 host suite from `scripts/`, and a screenshot no document referenced.

Two things this file is **not**: a list of decisions (those live in the section
that owns them), and a second `approver-esp32/status.md` (that file is row-by-row
about the firmware and stays authoritative for it).

## What is verified green

Run before the list below was written, and again after every fix, so a defect is
never confused with a broken build:

| Tier | Command | Result |
|------|---------|--------|
| Python | `.venv\Scripts\python.exe -m pytest -q` | **453 passed, 40 skipped** (no NATS, no YubiKey, no board, no browser) |
| Rust | `cargo test -q` (in `statusline/`) | **59 passed** across four binaries |
| Rust format | `cargo fmt --check` | clean |
| Node | `npm test` (in `approver-web/`) | **38 passed** |
| Browser tier | `scripts\web-approval.cmd` | **16 passed** in ~35s (needs NATS and `agent-browser`) |
| Host tier (ESP32) | `scripts\esp32-host-tests.cmd` | **724 passed, 0 failures** — run rather than counted, which is the honest form of this row: the 688 it held before was a `RUN_TEST` grep, and §10.16's gate plus §10.9's AP assertion added 27 tests on top of what that grep saw |
| Parity vectors (ESP32) | `scripts\make-vectors.cmd --check` | up to date |
| Markdown links | every relative link in every tracked `.md` | all resolve |

---

## 1. How these documents drift

The pass that opened this file found nineteen places where a document had gone
stale against its own code. All nineteen are fixed and the list of them is in git
rather than here — but the *shape* of them is worth keeping, because three kinds
of drift accounted for nearly all of it and none is caught by any test:

- **a count written as a number** — screens, modules, test cases, source files;
- **a "not done yet" note left behind by the thing getting done**;
- **a section that moved between files while its citations stayed put.** A
  `§10.5` in a comment names a section, not a file, which is exactly what let two
  of them rot quietly.

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

### 2.3 `approver-web` has no authentication on the page

The last of the four things that document listed, and the only one that was never
a gap in the code — it is a decision about where the app may run.

Anyone who can open the page in that browser profile can approve a `rm -rf`;
anyone who can reach the port can hand `POST /api/register` a token. Acceptable on
loopback, "unacceptable the moment it binds to anything but the loopback
interface". The same open question as §2.4's, one component over — and §2.4 has
since answered half of it, so the shape of an answer exists to copy: a credential
in the config that gates every route, off until it is set. `approver-web` runs in a
browser rather than on a device, so a password box is the cheap half there too.

Note that registering several browsers does not touch it: two browsers are two
keys, not two identities with different rights.

### 2.4 The ESP32 web server: authentication done, the interface half is not (§10.16, §10.3)

**The authentication half is closed.** HTTP basic auth is on every route of the
site — the pages, both API reads, the forms, the actions and the reboot — off until
`config.json` carries both a `web.user` and a `web.password` (`web login <user>
<pw>` on the console sets them). It cannot be changed from the site it locks, since
`web` is not on the write path's whitelist; 21 host tests cover the gate and the
encoder. `web.md` has the four rules and the size.

What is still open:

- **binding to the access-point interface only** — the second of the two ways
  `web.md` named, and it is not made redundant by the first: basic auth puts the
  password on the wire in base64, so on a LAN it is a speed bump rather than a
  boundary, and a listener that does not exist on that interface cannot have its
  password read off it at all. `web auto` is about the access point *being up*, not
  about which interface the socket binds to — nothing in the code decides this
  today. TLS on this port would be the other way to fix the same thing, and it is
  §2.5's company.
- **the Wi-Fi screen has no row saying whether the page is up** — `web.md` says
  "it should". The system status page has the row; §10.8.6's screen does not. The
  `web` console readout now has an `auth` line as well, so there are two facts that
  screen does not show rather than one.

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

Each of these is a thing no host test can reach, listed with what it needs. **Four
of the original six are done**, and a fifth since — all recorded where the
behaviour lives rather than here: the touch calibration's four crosses pressed and
the fit applied, the settings list dragged with a finger **and its `config save` row
pressed, `saved` watched for its three seconds**, `poweroff` actually switching
the board off, which needed the one session this repository cannot script: the
cable out, so on battery, with no console to watch it from — and **SNTP's six-hour
interval**, which needed only a board left alone for long enough and now has one:
a 13½-hour uptime with syncs at 00:10:57, 06:10:57 and 12:10:57 local, each moving
the clock `+0 s`. `status.md` has it, with the half that is still owed (a *failed*
sync, so the backoff, has never happened on this board).

| What | Needs | Where it is recorded |
|---|---|---|
| the **pressed half** of `tests/test_esp32_device.py` — the nine tamper assertions that come free with one press. **A press producing a reply `hook.verify_reply` calls trusted has now happened twice** (`test-request.cmd`, `TRUSTED`, this board's key), so what is left is running the suite that turns that press into the other nine | a finger, and `scripts\esp32-approval.cmd` | `tests.md` tier 3, `status.md` |
| Wi-Fi **auth-failure classification** (sticky, and spelled differently from "no such network") | a network whose password is deliberately wrong | `firmware.md` §10.9 |
| a **browser** on the configuration site of §10.16 — the credential dialog put up and dismissed. Every route was checked with `curl`, which is not a browser | a phone or a laptop on the same network | `web.md` §10.16, `status.md` |

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

### 2.8 Nothing of ours bounds an inbound message before the library allocates for it (§10.5, §10.14.1)

**The measurement this section used to ask for has been taken** (§10.14.1 has the
table, from a board) and it named the wrong worst case. Everything this device does
of its own accord — a card raised, the alert played, a reply signed, a Wi-Fi scan,
a page served, all at once — never went below its own **boot** floor. What costs is
one message arriving, at about **1.7× its size** in peak heap, because
`debsahu/espidf-nats` allocates a buffer for it before `ui::kToolInputSize` (2,048)
or anything else of ours is reached.

What is open is therefore narrower and more concrete than "measure it":

- **the boundary is a third-party allocation failure, not a rule.** When the
  allocator cannot satisfy the buffer the library fails the read, drops the socket
  and reconnects — politely, repeatedly, measured. That is why the low-water never
  approached zero (4,260 free was the worst seen, and it took a sequence of
  near-limit messages). But it means the thing protecting the heap is a dependency
  behaving well, and §10.5 has said since it was written that a cap of *ours* ahead
  of that allocation is the fix.
- **the safe size is not a constant and it is shrinking.** §10.5 recorded 64 KB
  delivered intact into 89,480 free bytes; a finished build with §10.16's site up
  has ~37,000 and the boundary is now ~15 KB. One variable proves it: the same
  15,634-byte message is accepted with that site down and refused with it up. Every
  feature added moves this number and nothing notices.
- **the number that predicts it is not reported.** Free heap and the low-water mark
  are in `status` and on the `system` status page; `heap_caps_get_largest_free_block`
  is the one that decides whether the next message lands, and nothing prints it.
  §10.14.1 already asks for the low-water on the About screen — this belongs beside
  it, and it is the cheapest item here.
- **the denial of service got cheaper, not dearer.** The server's
  `--max_payload=65536` bounds the megabyte case; a ~15 KB publish is under that
  bound and drops this responder's socket just as effectively (§10.10's scenario,
  no malformed frame needed).

### 2.11 Battery and sleep (§10.13)

Explicitly deferred: light sleep with a socket open, or waking on Wi-Fi, changes
the "is it connected" story of §10.8.1. The collision is already named — a screen
that blanks is fine, a *radio* that sleeps means requests arrive late or not at
all, and a responder that is asleep is a responder that times out.
