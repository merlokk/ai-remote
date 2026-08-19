# approver-esp32 — the tests, and the situations they pin

This file owns **§10.11** of the project docs: the three test tiers of this
firmware, what each one covers, and the mutation passes that say whether a test
is worth its line. Section numbers are global and stable
([`../CLAUDE.md`](../CLAUDE.md) §2), so §10.11 keeps its number here.

Where the rest of §10 lives:

- [`CLAUDE.md`](CLAUDE.md) — what this device is, its status, and the map;
- [`hardware.md`](hardware.md) — the board and the drivers the host tier fakes;
- [`firmware.md`](firmware.md) — Wi-Fi, the settings file, and the layering that
  makes a component testable with no board at all;
- [`protocol.md`](protocol.md) — the signing bytes, the exchange and the link
  policy these tests pin;
- [`screens.md`](screens.md) — the screens whose deciding halves are tested here;
- [`web.md`](web.md) — the configuration site, whose whitelist is tested here;
- [`build.md`](build.md) — the dependencies, and how the host suite is built;
- [`working-with-code.md`](working-with-code.md) — the one command that runs it.

The project-wide TDD rule is root [`../CLAUDE.md`](../CLAUDE.md) §1, and it is
the rule this file exists to serve rather than to restate.

### 10.11 Tests

Root §1's TDD rule applies here as it does to pytest, Rust and the Node half.
Three tiers, and the first one is where nearly everything belongs:

1. **Host tier — no board.** The pure logic runs under Unity on the development
   machine: the signing-bytes
   assembly, the reply builder, payload validation (not JSON, fields missing,
   values out of range, a `MSG` with no reply-to), the base64 helpers, the
   `int64` `ts` round-trip, and the config files of §10.15 with every field
   missing.
   The frame-level parser is no longer ours — it moved into
   `debsahu/espidf-nats` with §10.4's decision, so firing truncated and
   oversized frames at it is a device-tier job (§10.5). The **I²C lease** of
   §10.14.3 lands here instead, against its fake backend: contention between two
   holders, an acquire that times out, a recovery after a stuck slave — the
   library layer has no protocol in it (§10.14.2), which is exactly what makes
   it runnable on the host. This is the tier that has to be comprehensive,
   because it is the one that runs on every change.

   **It is not built by ESP-IDF, and that is a finding rather than a
   shortcut.** This section used to open with "ESP-IDF builds for the `linux`
   target", and the target *is* offered by the install here
   (`idf.py --preview --list-targets` lists it) — it just does not work on a
   Windows host: `set-target linux` picks esp-clang and then tries to link a
   Windows PE against `kernel32`/`user32` with `ld.lld`, ending in `unable to
   find library -lkernel32`. Measured, not assumed.

   So `host_test/` is a plain CMake project built by the host compiler, and it
   keeps the two things that matter. It is **Unity**, taken out of the ESP-IDF
   checkout rather than vendored, so these tests move to the `linux` target
   unchanged the day it works; and it needs **nothing installed** — MSVC is
   already here for §10.12.1's LVGL preview, CMake and Ninja ship with
   ESP-IDF. One command, in [`working-with-code.md`](working-with-code.md).

   **What is under it today is the navigator, every screen's arithmetic, all
   of §6 and §7's wire format, four of the five chips on the I²C bus, the
   settings file, the buttons, the zone table, the speaker, the Wi-Fi policy,
   the internet check, the clock's sync schedule, the bus link and the panel's
   idle timer** — 669 tests, and the last row of the table is tier 2 living in
   the same binary:

   | Subject | What is pinned |
   |---|---|
   | `components/ui` | every transition of §10.8's table; swipes that must *not* navigate on the settings, Wi-Fi or network-list screens; the settings screen being reachable from the clock **and from the limits, and from nowhere deeper** — §10.8.5 says what changed and what found it — and the Wi-Fi screen and the status pages only from settings, each backing out one level rather than to the clock; the network list **only from the Wi-Fi screen**, including not from itself, since that would be a way to reload it and lose the selection, and its own `kBack` landing on the record it was opened for rather than two levels out; a request preempting every one of those screens without moving any of them; navigation vanishing entirely while the card is up; the screen underneath surviving both an answer and an expiry; the pending queue refusing a fifth arrival. Its `CMakeLists.txt` has an empty `REQUIRES`, which is not an omission — a navigator that included LVGL would be a navigator that needs a board |
   | `components/ui` (the clock face) | §10.8.2's rules, and the sixth subject in this firmware that needs no fake. **The time**: an unset clock showing dashes rather than a plausible `00:00`, a year outside the RTC's own 2024..2099 refused at each end, the digits being 24-hour with their leading zeros, and a wall clock out of range showing dashes rather than rendering an hour of 74. **The three indicators**: a connected link never showing *no* bars, because an empty icon reads as "not connected"; the connecting animation never being blank; an access point and a switched-off radio both lighting none; no server configured being a shape rather than a red light; the two-minute traffic window opening on a delivery, closing on time, surviving the ~49-day wrap, **not** opening on the first reading of a counter that was already non-zero, and **not** opening on a counter that went backwards — which is a reconnect (§10.5) rather than a message; traffic never showing through a dropped link; and a battery percentage clamped, with negative meaning "nothing to ask" rather than empty. **The panel's own two**: the drift staying inside its box, reaching all four corners of it, never standing still, and being continuous across the millisecond wrap — the last of which is the only test that catches a cycle read as `now_ms % period`; and the water bounded away from both ends of the scale, travelling with the phase, varying down the face, and free of hard edges. Plus `Wave` interpolating rather than reading its 64-entry table flat, which is here because the mutation pass showed that breaking it was invisible through `Shimmer`. **And §10.15's notice**: shown when there is one, gone at its window to the millisecond either side of the boundary, and — the test that had to be strengthened before it tested anything — still right across the ~49-day wrap on **both** sides of it, since a window written `now < since + window` passes every check taken after the counter wraps and fails only in the moments just before |
   | `components/ui` (the request card) | §10.8.4's rules, and the ones where a test is worth the most. **What a card is allowed to be**: every refusal is its own case — a queue that is full, a field with no terminator in it (all seven, in one loop, so a field added later cannot skip the check), a request with no reply subject, one with no tool name — because a refused card is §10.10's fail-safe and a *shown* card is a question put to a human. **The queue**: the oldest is the one on screen, the bound is asserted equal to the navigator's, and room freed is room usable. **The press guard**: a press inside the first 300 ms is thrown away, a press that *began* before the card appeared is thrown away by the same comparison, the next card in the queue gets its own guard from scratch, and pressing nothing decides nothing. **The outcomes**: a press hands back the whole request the reply will have to echo; a card that timed out reads as a timeout and not as a deny; a request that waited past its own life is dropped without ever being shown; a missing TTL falls back to one that expires rather than to forever; the countdown floors at zero; and the ~49-day wrap lands inside a card's life. **The receipt**: it fades on its own, it is skipped when another card is waiting, and an arriving request outranks it. Plus the assertion the whole file is built around — **no amount of ticking produces a verdict** |
   | `components/web` | §10.16's whitelist, and the suite is almost entirely refusals — because the pages and `config.json` share one flat filesystem, so every one of them is about a WPA passphrase not leaving the device over HTTP. `/config.json`, `/registration.json` and every other `.json` refused because the extension is not on the list rather than because the name is on a blacklist; an unlisted extension refused; traversal in three spellings, including the percent-encoded one that nothing decodes; a name longer than SPIFFS can store refused **at the bound and one over it**; a buffer too small refused with the caller's buffer untouched (§10.2's rule about a refusal writing nothing); `/` being the index, with and without a query string; and the six files this site is made of all passing, `404.html` included — the server opens that one by name rather than from a URL, so nothing forces it through the whitelist and it is asserted to pass anyway. **And the reboot's confirmation**, which is a scan over the query rather than a `strstr` for one reason worth a test each: `xconfirm=reboot` and `confirm=reboots` both *contain* the word and neither confirmed anything, a path that spells it is not a query, and the comparison is case-sensitive like every other one in that file. Plus the content types, and that one is never null — that fallback is unreachable through the whitelist today, and is kept because "unreachable today" is not "unreachable". **And the write path** (§10.16), which is the only way anything outside this device changes what it does, so the suite is a list of things that must not get through: a field that is not on the whitelist refused **by name** — the touch calibration, the display, the clock, the audio, `wifi.rounds`, and `web.write` itself, which would otherwise make the read-only switch a suggestion — a mode that is not one of the three refused rather than guessed, a URL that will not parse, a string longer than its field refused rather than cut, a fifth network refused rather than dropped, a nameless one refused, an address with a leading zero and one with an octet over 255, and every one of them asserted to leave the settings **byte for byte** as they were. Then the four that are not refusals and carry the design: a password that was not retyped is kept **by name rather than by position**, an empty one clears it, a new network starts with nobody else's, and an address the form does not carry survives — that last one is a bug the board found by pressing Apply and watching a `192.168.1.42` disappear. Plus the four verbs, where a value that merely starts with one is not it. **And `web::ShouldRun`, which is four lines and two panics**: no network stack means no server in every mode, `off` stays off with a network, `on` is up on either kind of network, `auto` waits for an access point — and a radio that is merely *no longer wanted* takes the server with it while the stack is still up, which is the ordering that stopped `wifi mode off` rebooting the board |
   | `components/ui` (the Wi-Fi screen) | §10.8.6's, and the suite divides the way the screen does. **The mode**, which is the one thing here that can switch a radio on: every combination of the two config fields, `active` false being off whatever the mode says, the cycle reaching all three states and coming round, and both directions of the mapping — because it is the caller that writes those two fields back. **The record**: the access point being exactly one whatever the network list holds, switching to it showing that one immediately rather than a frame later, switching *off* leaving the record alone — off is a statement about the radio — the arrows wrapping both ways, one record and none being different sentences, and a network forgotten from the console pulling the index back rather than leaving it past the end. **The scan row**, which is the owner's "you cannot add a network" as behaviour: nowhere to put a name is its own refusal, and an access point always has somewhere. **The list**: a scan in flight, an empty flat and a refused scan as three states rather than one, a hidden network with no name dropped, the same name twice dropped, a crowded flat bounded, an SSID that fills its field surviving with a terminator, the window following a selection that wrapped, a tap on an empty row saying it selected nothing — because on that screen a tap also picks — a stale scan ignored, and the list dropped on the way out. Plus the two that are about coming *back*: a fresh visit starts at the first record and a pick leaves the index where it was |
   | `components/ui` (the settings list) | §10.8.5's rules, and nearly all of the value is in one row: **the two destructive rows are armed before they fire**, and every way of getting a single press to reach a restart or a shutdown is a case here — including the ones the second row added: a power-off refused while the cable is in and refused *before* it arms, an arming that belongs to the selected row rather than to the list, and the two rows being last and in the order of how hard they are to undo — the first press arming rather than going, the second one inside the window going, the arming expiring and the next press arming again, walking off the row clearing it *by either route* (a tap and the button take different paths, and only one of them was covered until a mutation said so), re-selecting the row that is already selected **not** clearing it — which is what makes a tap-tap work, since a touch reports both the selection and the press — leaving the screen clearing it, and the window measured across the ~49-day wrap on both sides of it. Then the list itself: it opens at the top, reboot is asserted to be the **last** row, the walk wraps and does **not** skip the rows with nothing behind them, a tap past the end selects nothing rather than clamping to an end it did not aim at, and a row with nothing behind it answers `kNotBuilt` rather than navigating. Plus the pager: three pages, wrapping, and back to the first when the screen is reopened. **And the two rows that reach the filesystem** (§10.8.5), where the value is in what they *do not* do: one press each and no arming, asserted in both directions, and their place in the order — after the screens, before the two that end the session. **The window**, because the list is longer than the panel: it is asserted that it *is* (`kEntryCount` against `kVisibleRows` — a suite that windowed a list which fits would be testing nothing), that the window stands still while the selection moves inside it, that it follows by one row rather than a page, that it comes back when the selection wraps, that the selected row is on the glass over *every* selection there is rather than at a sample, and — the one that would cost the most — that **a tap is a slot and not a row**: once the window has moved the two are different numbers, and using one as the other presses whatever is that far down the list. Then the outcome note: shown, expiring at its own millisecond, belonging to one row, replaced by the next press, dropped when the screen is left, and measured across the ~49-day wrap |
   | `components/ui` (the touch correction) | §10.8.5's, and the suite is mostly **refusals** — because a correction that is merely wrong is one the operator can see and redo, while one that is *badly* wrong takes away the screen it was made on. Every refusal is its own case and every one of them is asserted to leave the caller's calibration byte for byte as it was: an incomplete set, four presses in one place, a stretch nobody could want, a fit that would push a corner off the glass — and that they do not share a sentence, since "you tapped the same place four times" and "your screen is mounted sideways" send somebody in different directions. Then the arithmetic: identity changes nothing, a shift is recovered *and undone*, one bad tap out of four moves the answer by a few pixels rather than deciding it — which is why it is a least squares — a mirrored panel fits to a negative scale, and a corrected point is **clamped onto the panel**, because a point off the edge is one LVGL hit-tests against nothing. And the flow: the test mode records no points at all, a press too short or too long is not one, a fifth is ignored rather than overwriting a fourth, the result fades on its own across the ~49-day wrap, and abandoning a calibration leaves the one in use alone |
   | `components/ui` (the panel's idle timer) | §10.8.1's, and the twelfth subject in this firmware that needs no fake. **The two thresholds**: a dim at its own millisecond and not one before, reported as an edge rather than a level — the caller sends a QSPI command on every `true` — anything at all putting it straight back to the configured brightness, the wait measured from the last thing that happened rather than from the last time the screen was lit, and zero disabling either wait, which is an answer rather than a missing value. **The blank, which is the half with a condition on it**: it never happens lying flat however long the device is left, it happens standing on the USB edge, laying the board down while it is dark brings the screen back, and a hand-edited file whose sleep is shorter than its dim still switches the panel off — the stronger of the two statements winning rather than whichever is checked first. **And the two readings §10.13 says to measure rather than derive**: every one of that section's six positions, with the card-slot edge given as the numbers actually read off this board; a device on a corner or in a hand being *no* position at all, since the blank needs a statement and not the absence of one; a stand that leans back still counting; the noise of a board at rest not being movement, against the 0.02 g the magnitude really wanders; and a rotation about gravity being movement, which is what makes the check the whole vector rather than an axis at a time. **And the table itself, since it grew a second reader**: every one of the six positions naming itself, nothing dominant answering `kUnknown`, `StandingButtonsUp` asserted to be a *reading* of that table rather than a second opinion about it — the blank and the status page cannot disagree about which way up the board is — and every name short enough for the status page's value column, a bound this layer cannot see and which was **measured on the glass**: the first 21-character name was photographed and its rightmost lit pixel was column 479 of 479. Plus the pair that is about the console: a shorter wait typed in applying without an activity, and configuring **not** counting as one — and both waits measured across the ~49-day wrap, which for this subject is not a hypothetical, since reaching it means nobody touched the thing |
   | `components/buttons` (short and long) | §10.8.5's `KEY`: a press released early is short; one held to the threshold reports **long while the finger is still down**, which is the decision the class exists for; it reports it **once**, and the release afterwards says nothing — without which one press would open settings and immediately activate whatever row it landed on; the next press starts over; a button nobody is touching reports nothing at all; and the hold is measured across the ~49-day wrap |
   | `components/protocol` | §7's signing bytes (§10.2), and the suite with the least room to be approximately right — every other test here protects a behaviour somebody would notice going wrong, this one protects an exact byte string whose failure is invisible from the device's side. The complete messages have **moved to tier 2's generated vectors** in the row below, which is where they should have been: they used to be three literals pasted into this file, and a pasted literal is the one kind of expectation nobody can check for staleness. What is left here is the shape independently of the content — the two always-empty fields keeping their *positions*, which is what makes the message end in a separator and is the easiest thing to lose while tidying; exactly eight separators whatever the fields hold; and every refusal writing **nothing**, because a half-assembled buffer here is something a caller could sign. And the integers on their own, `INT64_MIN` included: the value with no positive counterpart, which the obvious negate-and-divide loop gets wrong and which is the reason `AppendInt` accumulates downwards |
   | `components/protocol` (the registration exchange) | §6/§10.7's, and the suite is mostly about **one rule**: the handler's signature is checked before any field of the reply is read. That is testable at all because the verifier comes in as an argument — the one used here *records the message it was handed*, so "these are `registration_reply_signing_bytes`" is an assertion rather than a reading of the code, and every rejection that happens *before* the signature uses a verifier that fails the test if it runs. Then each refusal as its own case, because each has its own sentence on a console: not an object, another protocol version, an `ok` that is a string rather than a bool, no handler key, a **pin mismatch** (which is not a bad signature and must not be spelled as one), a nonce that answers a different request, no timestamp, a `ts` past 2^53 that cJSON would silently round, a field longer than the device will hold, and a signature that is not one. Plus the two that are about agreeing with Python rather than refusing anything: the signing bytes byte for byte, and an absent optional field signing as `""` — because the handler omits `error` on success and the other side signs it as empty |
   | `components/protocol` (the wire format) | §7's JSON, and the fixture is **`hook.py`'s own output** rather than something written to match the parser — a request invented here would pass for a parser that agrees with this file and with nothing else. The parse: every field where §7 says, `tool_input` rendered **whole** rather than reached into (§10.8.4 forbids showing part of what is being asked for), and no TTL arriving so the card's own default is what expires one. Then every refusal as its own case, because this is the one subject anybody on the LAN can publish to: not an object, another version, a missing field one at a time, a field longer than the card holds, a `tool_input` too big to show whole — the refusal with a stated cost — a `ts` that cannot be echoed exactly, and no reply subject at all. Each of them asserted to leave the caller's card **untouched**, because the card in that struct belongs to a request somebody is still reading. The reply: the six fields `hook.py::verify_reply` compares one by one, an empty `reason`, and **no `updated_input`** — whose absence is what keeps `updated_input_sha256` empty in the signed bytes |
   | `components/ui` + `components/protocol` (the limits) | §9.2, §9.7 and §10.8.3, and the fixture is §9.7's own example document. **The two traffic-light scales**, both boundaries of each, and the assertion §10.8.3 asks for by name — that they cannot drift into each other, checked at every percentage from 0 to 100 rather than at a sample. `countdown()` against `render.rs`'s own cases, a reset in the past reading `now` rather than underflowing. **Absent is absent**: a document with no `rate_limits` is ordinary, and one window without the other is read. `ts` as the only required field, junk keeping the last good document, a percentage clamped and *rounded*, and a field too long **truncated** rather than refused — the one place in this firmware where that is right, and the test says so. Then the arrival rules: a document raises the screen, the minute is checked at both sides of its boundary and across the ~49-day wrap, a stream that keeps arriving keeps the screen, and a dismissal survives ten more documents but not the silence after them |
   | `components/i2cbus` | §10.14.3's three: contention, an acquire that **times out rather than blocks** (asserted as the tick count it asked for, which is why the fake mutex never sleeps), and a recovery that clocks SCL nine times and drops the device handles with the old bus. Plus the device table's per-device clock, its reopen-on-speed-change, and its refusal when full. And **the non-recursive mutex, from both sides**: `AddDevice` called while a lease is held is refused rather than granted — the trap `es8311.h` records — and `Recover`, which used to skip the lease entirely, now waits for it and tears nothing down if it cannot have it |
   | `components/pmic` (the power key) | §10.1's, and the newest of them: `Init` **writes** the press-on and press-off thresholds and the long-press enable when the chip holds something else, leaves them alone when it does not — the vendor's rail guard applied to a second register — and keeps the bits of `0x27` that are not its own. Then the one this suite exists for: **configuring the key never writes `COMMON_CONFIG` bit 0**, which is the soft power-off it shares a register with, so a read-modify-write that preserved it would take the board down inside `Init`; and a bit found already set at boot is *cleared*, because leaving it would arm the next write of that register. Plus the whole thing being a `Config` field, so a board wanting a longer press does not need this driver edited |
   | `components/pmic` | the 13-bit battery field against the 14-bit ones — the width that gives a *plausible* wrong voltage when wrong; the TS-pin silencing and the ADC read-modify-write; VBUS needing both status bits; `PowerOff` refusing over USB **and writing nothing**; `Read` being one snapshot rather than a dozen moments; and the two halves of the vendor's rail guard — a rail already at 3.3 V is not rewritten (DCDC1 supplies the C6, so a pointless write is a risk with no upside) and one at the wrong voltage is, with the bits above the field kept |
   | `components/rtc` | BCD both ways; the seven counters in one burst; the OS flag making a *successful* read untrustworthy, and being masked out of the seconds it shares a register with; the clock stopped and restarted around a write — including after a write that failed; and the century this chip does not have, so 2100 and 1999 are refused before the bus is touched while 2099 goes through |
   | `components/imu` | 0x6B, the inverse of the habit — and a *stranger* answering there not stopping the search at 0x6A, which is a different problem from silence and only one of them is a reason to give up; CTRL1's auto-increment, without which fourteen bytes are TEMP_L fourteen times; the range actually changing the scale; signed counts; tilt |
   | `components/audio` | the volume mapping where 0 is silence rather than full scale; clamping; the codec coming up muted; rates refused rather than approximated, and askable *without* the chip (`RateSupported`, which is what the speaker needs before it stops the channel); a codec that never identified refusing to be set rather than writing to I²C address 0x00 — and the two lease rules of §10.14.3 that this suite **found broken here and in the IMU**: a bounded number of leases across a configuration sequence, and zero milliseconds slept while holding the bus |
   | `components/config` | every test §10.15 asks for, and the one it spends the most words on: **the write that is not allowed to half-happen**. All three post-crash states are exercised — a leftover temp dropped, a temp with no `config.json` finished into place, and a clean save leaving nothing behind — against a filesystem where `rename()` refuses to replace, exactly as SPIFFS does. Plus: the **committed** `spiffs_image/` files parsed rather than a fixture, so an edit that breaks them fails here instead of on a flash; the password placeholder still being `CHANGEME`; a missing, truncated, non-JSON or oversized file all ending in a restore; `Reload` deliberately **not** restoring; `registration.json` untouched by one; unknown fields lost on the next write; a named zone filling in its POSIX rule; a string too long for its field refused rather than truncated (and the network it belonged to dropped with it); a negative number clamped rather than wrapped into a `uint8_t`; and **valid JSON that is not an object** — `[]`, `42`, `"hello"` — restored rather than read as an object with no fields in it. And §10.15's button, without a button: `RestoreAtBoot(false)` leaving the file **byte-identical** — nothing read and nothing written, which is the whole of "released early, nothing happens"; `RestoreAtBoot(true)` making `config.json` equal to `config.init.json` and the boot that follows reading it; the registration untouched by one; a missing defaults file and an unmounted partition each reported as a failure that **changed nothing**, because destroying a working config to report a missing default one is the worst of both; and the two outcomes not sharing a sentence |
   | `components/buttons` | the debounce, which is the only part of this with logic to get wrong: a window that starts when the level is *first seen*, a bounce shorter than it swallowed, a spike that settles back reporting nothing, and the millisecond counter wrapping at ~49 days being a subtraction rather than a special case. Above it: `active_low` per button, because §10.1 found `PWR` wired the other way round; `Init` adopting a button that is **already down**, which is §10.15's whole scenario; `Init` refusing a table with no rows or an unfilled `GPIO_NUM_NC` pin; and `HeldFor` reaching five seconds through a poll loop, giving up the moment it is released, and costing nothing on the boots where nobody is holding anything |
   | `components/timezone` | the table checked against itself — every name looks itself up, every rule passes `LooksLikePosix`, no name does, and an index past the end answers row 0 rather than reading past the array — plus the aliases people actually type (`Europe/Kiev`, `Asia/Calcutta`), a shared rule named after its family rather than after a city, and the one that matters most: **an unknown zone answering `nullptr` rather than a guess**, because §10.8.2's named failure is libc silently reading a misspelling as UTC |
   | `components/audio` (speaker) | mostly the RIFF parser, which is where the value is: a `LIST` chunk between `fmt ` and `data` walked past, an odd-sized chunk padded the way RIFF requires, a 40-byte `WAVE_FORMAT_EXTENSIBLE` header stepped over, compressed-but-in-a-.wav named apart from not-a-WAV, and three claims a file is not allowed to make about itself — a `fmt ` chunk too short to hold a format, a `data` chunk with nothing in it, and a chunk length that runs off the end of the file. Then what reaches the wire — the fake channel **captures the bytes**, so "it streamed the audio and not the header" is known rather than guessed; stereo and 8-bit refused without ever unmuting; a truncated `data` chunk played as far as the file goes; the codec muted again after a failed write; a rate change stopping the channel, retuning both halves, and starting it — and **a rate the codec cannot clock refused before the channel is stopped**, with the next file still playing afterwards, which is the bug that section below is about |

   | `components/wifimgr` (the internet check) | §10.9's second state machine, and the same shape as the first — `<cstdint>` and nothing else, so no fake: `unknown` for "no link", "switched off" and "not asked yet" alike; a round moving to the next target at once rather than next minute; two failed rounds to say offline and **one reply to come back**, asserted in both directions because a symmetric rule would be a different design; a dropped link going to `unknown` rather than `offline`; a late answer arriving after the link went being ignored; the target that answered going first next round; and the minute counted across the ~49-day wrap |
   | `components/timesync` | §10.8.2's schedule, and the fourth file in this firmware whose subject includes `<cstdint>` and nothing else: a device that has never synced being **due**, which is what makes "at boot" and "when the internet appears" one rule; nothing asked without an internet, and nothing asked twice at once; a link flapping four times inside the guard producing **no** syncs while the same link returning after a longer gap produces one at once; `date sync` overruling the guard but **not** the off switch; the retry being far shorter than the interval and the backoff both growing and capped, because either half alone is satisfied by a constant; a success clearing what was learned about a failing server; a stray result rescheduling nothing; `Configure` keeping a sync that really did happen; and the ~49-day wrap landing inside an interval |
   | `components/nats` | §10.5's two halves, and the fifth subject in this firmware that needs no fake. **Where**: the four spellings of an address that are accepted, and each refusal as its own case — a port that is not 1..65535, `ws://` / `wss://` / `tls://`, a path, credentials, a bracketed IPv6 literal, surrounding space, a host too long for the field — plus the one that turned out to be a real gap, that **nothing at all is written on a refusal**, host as well as port. **When**: nothing connected without a network; one attempt outstanding at a time; the backoff both growing and capped; a drop being neither a refusal nor an instant retry; a lost network and a switch-off both tearing the socket down and only the first coming back by itself; a changed address dropping what is up; `ConnectNow` beating the backoff but not the off switch; a result nobody asked for ignored; and the ~49-day wrap landing inside a backoff |
   | `components/config` (the clock) | `syncHours` read, round-tripped and **0 kept as off rather than floored** — the opposite call to `internet.intervalSeconds` next to it, and the difference is the point: a probe list with no interval is a flood, a clock told never to sync has said something. Plus the one the owner asked for: **no `sntp` in the file, and an empty one, both leave nothing to ask** rather than falling back to a compiled-in host |
   | `components/wifimgr` | every rule §10.9 states, and it needs **no fake at all** — the policy includes `<cstdint>` and nothing else, so this suite is the navigator's shape rather than the drivers': the round-robin and where a round begins; the backoff asserted as *both* growing and capped, because either alone is satisfied by a constant; the fallback AP after the configured rounds, held open by an attached station and restarted from the beginning when the last one leaves — and **not** held open by the manager's own "nobody is attached" report, which arrives on every pass and would otherwise mean an AP that never expires; the window's expiry clearing the sticky auth failures, since that window was the chance to fix them; an exhausted list going straight to the AP rather than waiting out its rounds; a drop while online being neither an auth failure nor an instant reconnect; the connect timeout; `SetDesired` being idempotent, because the manager re-asserts it five times a second; and the ~49-day millisecond wrap landing inside a delay |
   | `host_test/vectors` (tier 2) | the parity vectors, run against the assemblers that have to reproduce them: every §7 decision and every §6 reply, byte for byte, against what `approver/protocol.py` produced. Plus the assertions a generated fixture needs to be worth anything — that there **are** vectors (a suite iterating an empty array passes), that a renamed one answers null rather than being silently substituted, and that the two extreme timestamps and the non-ASCII field are still among them. The bound vector doubles as the check that `kSigningBytesMax` holds the largest message §7 can produce |

   The fake platform is `host_test/fakes/` — an ESP-IDF-shaped set of headers
   with a register-file I²C device behind them. It models the shape all five
   chips are (a write moves the cursor, a read takes from it), knows nothing
   about any particular one, and can be told to NACK, to fail the *n*th
   transfer, or to hand the bus to another task. §10.14.3 records why it is
   headers rather than the interface that section originally specified.

   **Every one of these was mutation-checked rather than trusted**: break the
   rule, watch the test that covers it fail, put it back. **Twenty by the end of
   the pass described here**, and the number is the length of the list that
   follows rather than a running total for the suite — the three passes recorded
   further down (the clock's schedule, the Wi-Fi policy, the bus link) add
   twenty-six more, and nobody should be keeping a tally honest across four
   paragraphs. The twenty: the card-outranks-navigation rule, the lease
   timeout, `Recover`'s handle drop, the battery field width, `PowerOff`'s
   refusal, the RTC's OS flag, and `config`'s three: the boot-time recovery,
   the restore-on-bad-file, and the atomic write; plus `Init` adopting a held
   button, `active_low` being per button, and the zone lookup refusing to
   guess. The last eight are the ones the pass below added: the speaker's
   rate check and its rollback, the short `fmt ` chunk, the empty `data`
   chunk, `config`'s object check, `Recover`'s lease, `AddDevice`'s lease,
   and the codec's `present_` guard.

   Four more needed no mutation, because they failed on the real code the
   first time they ran: §10.14.3 has two, §10.8.2 the third, and §10.8.1 the
   fourth.

   **The Wi-Fi screen added twenty, all caught, and one of them had to be aimed
   somewhere else before it was a mutation at all.** The nineteen that went
   straight through: `active` ignored so that looking at the screen switches the
   radio on, the mode cycle never reaching off, a press on the mode row not
   moving the record with it, switching *off* moving it, the access point counted
   as many records as there are networks, the index left past the end of a list
   the console shortened, the scan list opened with no record to fill, a stale
   scan accepted, a hidden network listed as a nameless row, the same name listed
   twice, a full SSID losing its last character, both halves of the window
   following the selection, a tap on an empty row claiming it selected something,
   the note window written the non-wrap-safe way, an empty list picked from, a
   fresh visit keeping the last one's record, and the navigator's two: the list
   opened from settings and its way out skipping the Wi-Fi screen.

   The twentieth is the useful one, and it is a trap §10.11 already records once:
   `CanStep()` lives in the **header**, so a mutation aimed at the `.cpp` found no
   pattern — and a mutation that reports "not found" is not a survivor and must
   not be counted as one. Aimed at the header it is caught at once, by the test
   that says one record is nothing to step through. The harness `os.utime`s every
   file it restores, for the reason the paragraph above gives.

   **The clock's schedule added six**, all caught, and they are the six rules
   §10.8.2 spends its words on: the flap guard removed (a link bouncing then
   syncs per bounce), the backoff made constant, the backoff's cap removed,
   `Configure` forgetting a sync that happened (a shorter interval typed into
   the console would then re-sync at once, which is how a settings command
   becomes a way to hammer a server), a result nobody asked for being
   accepted, and the wrap-safe deadline comparison replaced with `now >= at`
   — that last one caught by the ~49-day test and nothing else.

   **The Wi-Fi policy added nine mutations and, more usefully, two
   survivors** — and §10.11's rule held: a mutation that survives is a
   question about the code, not a gap in the tests.

   - **`max(1, rounds_before_ap)` was guarding nothing.** Removing it changed
     no test *and no behaviour*, because the round is incremented before it is
     compared: `rounds: 0` was already a full pass over the list. The helper
     was deleted and the invariant written down where the comparison happens,
     which is worth more than a function that looked like it was defending
     something.
   - **"Try last-successful first" had no test at all.** It survived because
     the drop-while-online path sets the network directly and never goes
     through `RestartClient` — so the only caller of the rule was `wifi mode
     off` followed by `wifi mode client`, which nothing exercised. That is now
     a test.

   A third mutation would not compile, for the reason this section already
   records: `/W4 /WX` makes the code a mutation orphans into an error, so
   short-circuiting a branch has to leave nothing unreachable behind it.

   **The clock face added fourteen, all caught, and one of them only after the
   test was rewritten.** Thirteen went the ordinary way: the dashes, both ends
   of the believable-year range, a weak signal lighting no bars, the traffic
   baseline, the backwards counter, the two-minute window, traffic showing
   through a dropped link, the cycle read off the absolute millisecond counter,
   the water's floor, the drift collapsed onto one diagonal, the drift standing
   still, and a connecting animation showing nothing.

   The fourteenth is the useful one. Reading the sine table **flat** instead of
   interpolating it survived — and it survived because the smoothness test's
   threshold was loose enough to admit both: the difference between the two is
   about three units of intensity out of 255, which is invisible through
   `Shimmer` and would have been invisible on the glass. §10.11's rule says a
   survivor is a question about the code, and the answer was that the property
   needed pinning one level down: `Wave` is public now, and the test counts how
   many distinct values it takes over its 256 angles — 64 entries read flat
   cannot produce more than 64, and interpolation produces most of the scale.
   The claim in the code and the assertion in the test are the same claim now.

   **The request card added sixteen, all caught, and two of them only after
   something was fixed.** The fourteen ordinary ones: a full queue taking a fifth
   card, the field-length check skipped, a card with nowhere to answer shown, a
   card with no tool shown, the guard set to zero, the guard made unsigned so it
   could not see a finger that was already down, a timeout recorded as a deny, a
   timeout counted as an answer, queued cards never aged, a missing TTL meaning
   forever, the deadline compared unsigned, the next card inheriting the last
   one's guard, an arrival not outranking a receipt, and the decision handed back
   without the request it is about.

   The other two are about the harness rather than the code, and both are worth
   knowing before somebody repeats them. `Reached` lives in the **header**, so
   the mutation aimed at the `.cpp` found no pattern — a mutation that reports
   "not found" is not a survivor and must not be counted as one. And the
   "a receipt must not sit in front of the next card" mutation made `Front()`
   return null, which the tests **dereferenced**: the suite died with no output at
   all, which the harness classified as a build error. The mutation was caught
   either way; the fix is a helper that answers `<no card>` instead of crashing,
   so the failure now names the rule that broke. A suite that dies without saying
   which test it was is a suite that costs an hour the next time.

   **The signing bytes added seven, all caught, and no survivors** — which is
   unusual enough here to be worth a sentence about why. Every rule in that
   file is a rule about an exact byte, so each mutation has a test whose whole
   job is that byte: the empty `updated_input_sha256` dropped, the trailing
   separator dropped, `AppendInt` negating first instead of accumulating
   downwards (caught only by `INT64_MIN`), the behaviour allowlist bypassed, a
   too-small buffer truncated into rather than refused, a missing field read as
   an empty one, and the length bound off by one. The last of those is the one
   that needed the *test* fixing first: it originally pushed a 255-character
   string at a 63-character field, which an off-by-one bound refuses just as
   happily as a correct one. Exactly at the bound and exactly one over is the
   only pair that says anything.

   **The limits added eleven, ten caught, and the survivor is the useful one.**
   The ten: the quiet window off by a tick, a dismissal never cleared when the
   stream stops, a dismissal cleared by every arrival, the context gauge given the
   window's scale, the sub-minute countdown boundary moved, the countdown always
   taken from the publisher, a gauge with no reset given one anyway, a percentage
   left unclamped, a percentage truncated rather than rounded, and a document with
   no `ts` accepted.

   The survivor is **writing straight into the caller's document instead of into a
   local**, and the answer is that today it cannot matter: `ts` is the only
   required field and it is read first, so after that line nothing can fail and
   there is no input that gets half-written and then refused. The local is defence
   for the day a second required field is added below it. Kept, at about 300 bytes
   of stack, and the reasoning is now in `status.cpp` next to the line — because
   the alternative is somebody rediscovering the same survivor in a year and
   deleting the protection.

   One of the eleven was also a lesson about the ritual rather than the code: the
   first version of the dismissal mutation *inserted* a dead `if (false)` in front
   of the real clear rather than removing it, and duly "survived". A mutation that
   does not change behaviour is not a survivor; it is a mutation that was not
   written yet.

   **The boot restore added five, all caught, and the two that did not go
   straight through are the two lessons this ritual keeps re-teaching.** The
   three ordinary ones: a failed restore reported as a success, a failure that
   writes the defaults over the settings anyway, and the notice that never
   expires. The other two:

   - **"ignore the button and restore on every boot" would not compile.**
     `if (false)` orphans the parameter and `/W4 /WX` turns that into an error —
     the same trap §10.11 already records twice. `if (!key_held && false)` keeps
     consuming what it stops using, and then it is caught by the test that says a
     boot with nobody holding anything leaves the file byte-identical.
   - **"the window is not wrap-safe" survived, and the test was what was
     wrong.** Writing it `now < since + window` instead of `now - since <
     window` passes every check taken *after* the counter has wrapped, because
     the sum wraps with it; it fails only in the moments just before, where `now`
     is enormous and the sum is small. The original test picked its instants on
     the far side of the wrap and never asked the question. Both sides now, and
     the mutation fails as it should — which is §10.11's usual finding arriving
     from its less usual direction: a survivor that is a question about the test.

   **The touch correction added sixteen, all caught in the end, and two of them
   are the useful kind — a survivor that was a question about the code and one
   that was a question about the test.** The fourteen ordinary ones: a corrected
   point left unclamped, a refused fit written out anyway, an incomplete set
   fitted, a corner pushed off the glass accepted, the scale check reading the
   sign as well as the size, the four crosses put in one corner, a press of any
   length taken as a point, a fifth press overwriting a fourth, the test mode
   recording points, the result never fading, the result window written the
   non-wrap-safe way, giving up keeping the points collected so far, and the
   touch test opening from anywhere or being swiped away from.

   The two that mattered:

   - **the scale check was unreachable, and a surviving mutation is what said
     so.** The span guard was written as "the raw span is at least half the
     target span", which *is* the statement "the scale is at most two" — so it
     fired first every time and the plausibility check below it was dead code
     that looked like a safety net. Pulled apart into an eighth and a factor of
     two, each now decides something the other cannot;
   - **and the test for the span guard could not tell it was gone.** Squeeze the
     four presses hard enough and the fit overflows an `int16_t` scale and is
     refused by the arithmetic instead — the right outcome and the wrong test,
     because it passes with the guard deleted. The divisor is chosen now, and
     the test says why.

   **The power-off row added five, all caught**: a shutdown happening with the
   cable in, a blocked press arming the row anyway, power off firing on one
   press, only the reboot counting as destructive, and the arming surviving a
   step between the two destructive rows. Nothing surprising, which is what a
   second instance of a machine that already has tests should look like — and
   the sixth mutation was written by the compiler rather than by hand: adding the
   row made the settings list 526 pixels tall on a 480-pixel panel, and
   `settings_screen.h`'s `static_assert` refused the build. A layout constant is
   the one kind of mistake that looks fine in every test and shows up only on the
   glass.

   **The settings list added sixteen, all caught in the end, and the interesting
   part is that three of them were badly written the first time.** The thirteen
   that went straight through: one press on reboot restarting, a tap on the armed
   row disarming it, the arming never expiring, the arming window written the
   non-wrap-safe way, coming back into settings keeping it, a tap past the end
   clamping instead of being ignored, an unbuilt row navigating anyway, the pages
   not wrapping, the status keeping its last page when reopened, a swipe
   navigating away from the status, the limits screen losing its way into
   settings again, a long press waiting for the release, and a long press also
   reporting a short one on the way out.

   The three that had to be rewritten are the ritual's own failure modes, and
   two of them this document already names:

   - **"the arming never expires" would not compile.** Removing the window
     orphans `now_ms`, and `/W4 /WX` turns that into an error — the trap §10.11
     records three times now. A mutation has to keep consuming what it stops
     using;
   - **"the status opens from anywhere" changed nothing.** It added a transition
     from the status screen *to* the status screen, which is not a behaviour. A
     mutation that does not change behaviour is not a survivor; it is a mutation
     that was not written yet. The real one — the clock opening the status
     directly — is caught at once;
   - **and one was a real survivor, which is the useful kind.** "Walking off the
     reboot row leaves it armed" survived a correctly-written mutation, because
     the test that covered it went through `Select` — the *tap* route — and the
     button route goes through `Next` and never calls `Select`. So arming the
     row, pressing `BOOT` four times to come back round to it, and pressing `KEY`
     would have restarted the device on what the operator reads as a first press.
     §10.11's rule is that a survivor is a question about the code; here the
     answer was that the code was right and the test was aimed at the wrong door.

   **The wire format added nine, all caught, and none of them interesting** —
   which is itself worth one line: a request with nowhere to answer shown anyway,
   any version accepted, a missing field read as empty, the tool input rendered
   with whitespace, a `ts` too large to echo accepted, the reply's nonce not
   echoed, any behaviour reaching the wire, an `updated_input` added, and the card
   written into before the message was known to be good. Nine rules, nine tests,
   no survivors and no surprises. A file that is nothing but field layouts is the
   one place where that is the expected outcome rather than a suspicious one.

   **The idle timer added fourteen, all caught, and no survivors** — which for
   this subject is what it should look like, because every rule in the file is a
   comparison and each has a test whose whole job is that comparison: the dim
   never firing, the dim off by a millisecond, zero no longer disabling either
   wait, the blank ignoring which way up the board is, the dim checked before the
   blank (so a hand-edited file with the shorter sleep would only dim), the state
   change made a level instead of an edge, the dim level left unclamped, the
   elapsed time computed the non-wrap-safe way, standing up decided without the
   sign, any axis being allowed to name a position, no axis having to dominate,
   movement checked an axis at a time, and the noise floor removed.

   Two of them are worth naming for what they would cost on the desk rather than
   in a test. **The sign** is §10.13's warning made concrete: with `y > 0.0f`
   replaced by `true`, a board lying flat on its back blanks itself, and the only
   symptom is a clock that keeps going dark. And **the noise floor** removed makes
   every rest-state jitter a movement, so nothing ever dims at all — a feature
   that silently does not happen, which is the class of bug this whole section
   exists for.

   **The registration exchange added nine, all caught, and one of them had to
   fix the test before it could be run.** The nine: the pin never firing, the
   fields copied out before the signature is checked, a truthy `ok` taken for a
   boolean one, the nonce echo not compared, the exact-integer bound on `ts`
   dropped, an absent optional field treated as a failure, the `ok` flag signed
   as `1`/`0` rather than `true`/`false`, a token split at the last dot instead
   of the first, and a field longer than its buffer truncated rather than
   refused.

   Two would not compile at first, which is `/W4 /WX` doing its job — removing
   the pin check orphans its parameter — and the fix is the one this section
   already records: a mutation has to keep consuming what it stops using, so the
   check stays and its condition is made never to fire.

   **And the `ts` one is the useful run.** The test written for it asserted that
   2^53+1 is refused *and* that 2^53 is accepted, and it failed on the untouched
   code — correctly, because the two parse to the **same `double`** and are
   indistinguishable from that point on. No rule can take one and refuse the
   other. So the bound became exclusive: below 2^53 every integer is its own
   value, at it ambiguity starts, and both are refused. Which also settled
   whether the bound is redundant next to the truncate-and-compare round trip
   below it — it is not, because the round trip compares the double with itself
   and cannot see a value that was rounded before it ever arrived. A test that
   was wrong is what established what the code should do, which is the opposite
   of this section's usual direction and worth the paragraph.

   **The bus link added eleven, one survivor, and a warning about the ritual
   itself.** Ten were caught — the backoff's growth and its cap taken away
   separately, an instant retry after a drop, the teardown that a lost network
   and a switch-off each ask for, a result accepted without being asked for, a
   stale restart kept, `Reached` written as `now >= at`, another scheme
   stripped instead of refused, and a port with trailing junk taken. The
   survivor was **writing the host before the port had been agreed**, and it
   survived for the ordinary reason: `AssertRefused` checked that the port had
   not moved and said nothing about the host, so half an endpoint could be
   overwritten by a URL that was rejected. The assertion now covers both
   fields, which is the test being wrong rather than the code — the other
   direction from §10.11's usual finding.

   And the warning, because it wasted twenty minutes and will do it again:
   **a mutation harness has to touch the file it restores.** Ninja goes by
   mtime, and a restore that puts back an *older* timestamp than the object
   built from the mutated source leaves that object in the binary. The symptom
   is a suite that fails a test nobody has touched — here the ~49-day wrap
   test, still linked against `now >= at` three mutations after that one was
   reverted. Every result above was re-taken with an `os.utime` after the
   restore; the first run's failure counts were inflated by exactly this.

   **Four bugs came out of the pass that asked "what else can a caller do?"**,
   and the shape they share is worth more than any of them individually:
   each is a **decision taken after something had already been changed**,
   where the fix is to decide first and touch nothing until then. §10.7 states
   that rule for `poweroff` and §10.8.2 for a bad date; these are the places
   it had not been applied.

   - **A WAV at a rate the codec cannot clock left the speaker dead until a
     reboot.** `Reconfigure` stopped the I²S channel, retuned it, and only
     then asked the codec — which refuses anything outside its five rates. The
     channel stayed stopped, and the *next* file matched `sample_rate_` and
     skipped the reconfigure entirely, so every playback after it failed. A
     22 050 Hz alert sound is a plausible thing to drop into
     `spiffs_image/`. It asks first now, and rolls the clock back if the
     retune fails partway.
   - **`Bus::Recover` skipped the lease.** It removes every device handle and
     deletes the driver — the most destructive thing in that class — while
     another task could be mid-transfer with a handle it had already been
     given. One core with preemption makes that a use-after-free rather than a
     race that usually works. It takes the lease now (§10.14.3), waits longer
     than an ordinary acquire because the holder it is waiting on is the stuck
     one, and tears nothing down if it cannot have it.
   - **A `config.json` containing valid JSON that is not an object read as an
     empty config.** `[]`, `42` and `"hello"` all parse; every field lookup
     then answers null, so the device came up on the defaults, said nothing,
     and left the file to do the same next boot. §10.15's restore is for
     exactly this.
   - **The codec's setters checked for a bus and not for a chip.** After a
     failed `Init`, `address_` is still 0, so `SetVolume` went out to I²C
     address 0x00 — the general-call address — and took a slot in the bus's
     fixed device table for a chip that is not there.

   And two the WAV parser was handing back rather than refusing: a `fmt `
   chunk shorter than sixteen bytes, which left `bits` reading 0 out of a
   zeroed buffer and presented it as a format the file had stated; and a
   `data` chunk of zero length, which "played" by unmuting, waiting out the
   drain and muting again — a click for nothing.

   **One mutation survived, and that was the useful one.** Hard-coding the WAV
   parser's `data_offset` to the canonical 44 changed nothing — because
   `PlayWav` never read it, and streamed from wherever the parser happened to
   leave the file. Correct, and correct by accident: the coupling was invisible
   and would break the first time the parser looked ahead. `PlayWav` seeks to
   the offset now, and the mutation fails as it should. A mutation that
   survives is not a gap in the tests; it is a question about the code.

   One of those mutations would not compile, which is worth knowing before
   somebody spends ten minutes on it: `/W4 /WX` turns a now-unused variable
   into an error, so a mutation has to keep consuming what it stops using.

   Two things the host build costs, both recorded because they will look
   arbitrary later. It needs **`managed_components/`** — the config tests use
   the real cJSON, which `idf.py build` fetches and git does not carry, so a
   fresh checkout builds the firmware once first. And a handful of POSIX names
   MSVC spells differently (`setenv`, `localtime_r`, `timegm`, `strcasecmp`)
   are supplied by a force-included header rather than by an `#ifdef _WIN32` in
   the firmware: the sources under test are the ones that ship, and that rule
   is worth more than the tidiness of not having the header.

   Three things the screens add to it, all of them logic rather than pixels, and
   two of the three are now in the table above: **the navigation state machine**
   (a request preempts every screen; it cannot be dismissed; what was underneath
   comes back with its state — §10.8.1) and **the Wi-Fi state machine** (backoff
   bounded, auth failure sticky and distinct from "not found", `forget` returning
   to `NO_CREDENTIALS` — §10.9) both are, and the clock face joined them. What is
   still owed is **the limits screen's arithmetic**: `countdown()` against
   `render.rs`'s cases, the staleness threshold, and the §9.2 gauge scales pinned
   exactly as `render.rs` and `statusline.ts` pin them, including the assertion
   that the two scales cannot drift into each other. Note the one the clock face
   already shares with that screen: its two-minute traffic window is §10.8.3's
   staleness threshold, and the two must not drift apart either.
2. **Cross-language parity vectors**, mirroring what `approver-web` does with
   `protocol.test.ts`: fixtures generated by the Python implementation itself and
   compiled into the host tests, so "does the device still speak §7?" is a
   question a test answers. Never hand-typed from this document.

   **This exists now, and the shape it took is not the one this section
   sketched.** The sketch was a fixture file; what it needed to be was a fixture
   file *plus a test on the other side of the repository*, and the second half is
   the whole reason the tier is worth having.

   | Piece | Where |
   |---|---|
   | the generator | `tools/make_vectors.py`, run under the venv (`lib/crypto.py` needs `cryptography`) |
   | §7's decisions and §6's replies | `host_test/vectors/parity_vectors.h` — generated, **committed** |
   | §10.6's Ed25519 vector | `components/crypto/selftest_vector.h` — generated, committed, and included by `device_key.cpp` |
   | the device reproducing them | `host_test/test_vectors.cpp` |
   | **the vectors still being current** | `tests/test_esp32_vectors.py`, in the pytest suite |

   - **Committed, the way `dependencies.lock` is.** A fresh checkout builds and
     runs the host tests with no Python step at all; the generator is something
     you run when `protocol.py` changes, not something the build depends on.
   - **A fixture without a staleness check is a pasted literal with extra
     steps**, and that is what this file previously had in three places. Change
     `signing_bytes` and a compiled-in expectation goes on passing against
     yesterday's layout forever — while the device signs bytes the hook does not
     recompute, every reply is rejected, and from the desk that is
     indistinguishable from a responder that is not answering. Nothing logs it.
     So the pytest guard regenerates both headers into memory on every run and
     compares whole files: a vector that quietly stopped being generated fails
     too, which a field-by-field check would not catch.
   - **What is in them.** Six decisions: a realistic `Bash` allow, a deny with a
     negative `ts`, `INT64_MAX` and `INT64_MIN` — the two values a `double`
     cannot carry — every field at its declared bound with both integers at their
     widest, and a `session_id` that is **not ASCII**, because `approvals.*` is
     open on the LAN (§10.3) and the device copies bytes while Python encodes
     utf-8. Six replies: an acceptance, a signed rejection, an anonymous one, a
     maximum `ts`, every field at its bound, and an `error` with a separator
     inside it — the vector that makes "`error` is last because it is free text"
     load-bearing rather than decorative.
   - **The counts are asserted.** A suite that iterates an empty array passes, so
     a truncated header or an include that resolved elsewhere has to fail rather
     than report a green run over nothing.
   - **Mutation-checked from both ends**, which is the only way to know a
     two-language check is wired up: swapping `session_id` and `nonce` in
     `protocol.py` fails the pytest guard (and `--check` names the stale file);
     dropping the trailing separator in `signing.cpp` fails
     `test_every_decision_vector_is_reproduced` with the vector's name in the
     message.
   - And one rule that came from the generator rather than from the tests:
     `INT64_MIN` and `INT32_MIN` **cannot be emitted as digits**. C++ reads
     `-9223372036854775808` as unary minus on a constant that does not fit, which
     under `/W4 /WX` is an error and inside a braced initialiser is ill-formed
     outright. The generator knows to write `INT64_MIN`, and that is the kind of
     thing a hand-pasted literal never taught anybody.

   **And one guard on this tier that has nothing to do with the protocol.**
   `tests/test_esp32_web_pages.py` reads the six pages of §10.16 off the disk and
   checks how they are *wired*: no page carries an inline `<script>`, each loads
   `app.js` exactly once, each `data-page` has a handler behind it, every element
   that handler asks for by id is on that page, and every link between pages
   resolves to a file that ships. It lives on this tier rather than in the Unity
   suite because the pages are assets rather than code — nothing in
   `components/web` reads them, and the C++ side can only say what a URL may
   *reach*, never whether what it reaches works.

   It exists because of a page that shipped broken and could not say so:
   `wifi.html` had a truncated inline copy of its own script, and an unterminated
   script block ends at the *first* `</script>` a parser meets — which was the
   closing half of the shared `<script src="/app.js">` on the next line. So the
   tag that loads the stylesheet and every page's logic was swallowed as script
   text, and the page came up with none of the CSS, none of the fetches, no error
   anywhere and nothing in the device's log. Its only symptom was that it did not
   look like the other five. Mutation-checked the honest way, against the file
   that actually shipped: the broken `wifi.html` out of git fails two of the five,
   and the fixed one passes.
3. **Device tier — opt-in, like §8.6's touch tests.** A board, a real bus, and a
   real `hook.py`: the acceptance test is a device-signed reply that
   `hook.verify_reply` returns `trusted=True` for, plus the same reply with
   `behavior` flipped being rejected.

   **This exists now** — `tests/test_esp32_device.py`, reached by
   `scripts/esp32-approval.cmd`. It is the acceptance test for this whole folder,
   and the argument for it is that everything else here can be green while the
   object on the desk is useless: the host tier compiles the firmware's logic
   without the firmware, tier 2 checks byte strings without a signature, and
   neither has ever seen the key bound to this chip.

   - **Two interactions, and the second is the one no host test can reach.**
     Press `ALLOW` on the card; then press *nothing* for twenty seconds, which is
     §10.10 — no press must mean no reply at all, not a deny and not a "skipped".
     A `RequestTimeout` is the pass.
   - **Every tamper is free.** Once one genuinely signed reply exists, the
     verdict flipped, each of §7's six echoed fields changed in turn, the
     `key_id` renamed to another allowlist entry and to none, and the signature
     removed are all in-process assertions. That is why two presses buy nine
     tests.
   - **`AI_REMOTE_ESP32_DEVICE=1`, and the gate is not a probe.** The YubiKey
     tier can ask whether a key is plugged in; there is no read-only way to ask
     whether this board is answering — the only probe is a request, which raises
     a card. Worse, §6's queue group means that with the board off and
     `responder.py serve` up, this suite would pass beautifully against the
     software key. Hence the env var, and hence asserting `key_id`, which is not
     a formality here: it is what says who answered.
   - **What has actually run**: the fail-safe half, against this board, on the
     bus — a card nobody pressed produced no reply in 20 s. The pressed half
     needs a finger.

