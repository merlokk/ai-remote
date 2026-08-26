# approver-esp32-yubikey — the configuration site on the device

This file owns **§10.16** of the project docs: the `esp_http_server` behind
`components/web`, the seven pages it serves off SPIFFS, the desired-state switch
that brings it up and down with the network, the whitelist that keeps a WPA
passphrase — and an ARKG enrolment — off the LAN, the write path and what it
refuses, the password on the door, and the numbers. Section numbers are global and
stable ([`../CLAUDE.md`](../CLAUDE.md) §2), so this is the same §10.16 the sibling
folder's [`web.md`](../approver-esp32/web.md) owns, because it is **the same design
on different hardware**.

Two rules from elsewhere bind everything below, and neither is restated as though
it were new: **nothing here can reach a verdict** (§10.10 rule 4, in
[`CLAUDE.md`](CLAUDE.md)), and the **trust boundary is the router** (§10.3, same
file) — so `web.write` is a switch and not authentication. There *is*
authentication: a user and a password in `config.json` put HTTP basic auth on
every route. It is a lock rather than a switch, it is still not TLS, and the
section below says both in as many words.

Where the rest of §10 lives:

- [`CLAUDE.md`](CLAUDE.md) — what this device is, its status, and the map;
- [`firmware.md`](firmware.md) — the settings this site writes and the Wi-Fi
  manager whose tick it borrows;
- [`protocol.md`](protocol.md) — the approval path this site may not reach;
- [`led.md`](led.md) — the one output this site reports on and cannot change;
- [`key.md`](key.md) — the security key this site cannot ask for a touch from;
- [`hardware.md`](hardware.md) — the board it reports on;
- [`tests.md`](tests.md) — the whitelist, the gate and the write path, as refusals;
- [`build.md`](build.md) — what it costs in flash, and where it came from.

## 10.16 The configuration web server — and here it is the only way in

`components/web`, and `web` on the console. **It is the sibling board's server,
and it is here for a different reason.**

There, §10.8.6 arrived at an on-screen keyboard 6 mm wide and named a phone's
keyboard as the affordable alternative: the site was a *nicer* way to type an
SSID than the glass. This board has no glass. Before this existed, the only ways
to give this device a network were the CH343P console and a `config.json` written
by a full reflash — and a full reflash on this board costs the key enrolment
(§10.18.1, and [`working-with-code.md`](working-with-code.md) is blunt about it).

So on this device the page is not a convenience. It is **the only way in that
needs no cable**, which is also why `auto` is exactly the right default: the
server comes up while this device is its own access point, which is precisely the
state in which somebody has no other way to reach it.

What that changes about the design is nothing at all, and that is the finding
worth recording: `web_paths.cpp`, `web_policy.cpp` and `web_auth.cpp` are the
sibling's files with the realm changed, and the two crashes and one double free
that shaped them (§10.16 next door) did not have to be re-learned here. What
differs is listed once, below, and then not repeated.

### What differs from the board next door

Six things, and each is a consequence of the hardware rather than a preference:

| | |
|---|---|
| **the realm is different, deliberately** | `approver-esp32-yubikey`, against the sibling's `approver-esp32`. An operator with both devices on one desk has two saved credentials in one browser, and the realm is the only thing in the dialog that says which device is asking |
| **there is no battery, so there is no gauge** | no PMIC on this board (§10.13), so `/api/status` carries no `battery`, `battery_mv`, `charging` or `usb`, and the front page has no power row. The sibling's rule that "a field that did not arrive is a dash" is kept for what remains |
| **there is PSRAM, so the memory row is two numbers** | 8 MB of it (§10.13), so `heap_free` is `MALLOC_CAP_INTERNAL` — the scarce pool that task stacks and `.bss` come out of — and `psram_free` travels beside it on its own field. That is the call `status` on the console already makes, and the reason is the same: a total of eight and a half megabytes would say nothing about the memory that runs out |
| **the light is a field** | `light` carries `indicator::StateName` — what the one emitter is saying right now (§10.17) — so the page and the light on the desk cannot disagree about what this device is. It is a **readout and there is no setter beside it**, which is §10.17's palette argument arriving on this side: an operator who can recolour `denied` can build a device that lies about what it did |
| **the key is two fields** | `key_present` and `key_enrolled` (§10.18). Not "does it work" — that is `key selftest`, it costs two curve operations and it asks for a touch, which is not something a browser gets to trigger |
| **`declined` is its own counter** | §10.10 rule 2 says a timeout is not a deny and the two are counted apart. On the sibling a press is cheap, so the interesting split was allow-versus-deny; here **every verdict costs a touch**, so "nobody touched the key" is the ordinary third answer. A page that summed it into `denied` would be reporting refusals nobody made, so the front page and the bus page each have an `unanswered` row |

And one thing that is the same shape for a *weaker* reason, which is worth saying
so nobody re-derives the strong one: **`web::kMaxSockets` is 3 and the pages are
still fetched one at a time.** On the C6 that was load-bearing — three sockets
able to queue 5,760 bytes each against about 18 KB of free internal RAM was a page
that hung, and §10.16 next door has that diagnosis. Here there are eight megabytes
behind lwIP's pools and no 48 KB graphics pool competing for the internal heap, so
the ceiling is not what keeps this device alive. It is kept because the pages are
the same pages: a site that only works on the roomier of the two devices is a site
nobody can move between them.

### The whitelist, and why it is sharper on this board

`web_paths.h` is unchanged, and the rule it enforces matters more here: **the same
flat SPIFFS namespace holds the pages, `config.json` *and* `fido.json`.**

That last file is §10.18's enrolment — the `ikm` and the key handle the ARKG
derivation needs. It is not a private key, which is why §10.6 can still say there
is no private key on this board; it is the half of the pair *this device*
contributes, and §10.18's property that "**the key alone cannot approve anything
either**" is exactly the claim that those bytes are not public. A `/fido.json`
served to a LAN would retire that property.

The whitelist is what makes it unrepresentable rather than remembered: it lists
**extensions**, `.json` is not one of them, and nobody had to think about this file
when it was added to that filesystem. `test_the_enrolment_is_not_a_page_either` is
that as three assertions, and it is the one test in the ported suite that is this
board's own.

### The write path, and the two sections a network may not touch

`web_settings.h` whitelists `wifi` and `nats` and refuses everything else **by
name**. On the sibling board the refused set was the touch calibration, the idle
timers, the clock and the display brightness — settings whose worst case is an
annoyance. Here three of the five sections of `config.json` are refused and two of
them carry §10.10:

* **`approval` decides when a verdict may be asked for.** `touchTimeoutSeconds` is
  how long a request waits for a fingertip; `denyButton` is whether BOOT can refuse
  one. A network that could set the timeout to zero could make every request expire
  unanswered — silence, which *is* §10.10's safe outcome, but silence an operator
  did not choose. A network that could switch the deny button off could take away
  the one refusal that costs no touch. Neither is reachable;
* **`led` decides what this device is saying while it asks.** The palette is
  compiled in (§10.17) and cannot be reached from anywhere; the brightness is a
  file field, and a brightness of zero written over HTTP is a device that answers
  requests with **no light at all**, on a board whose entire user interface is that
  light;
* **`web` itself**, which is the sibling's rule inherited unchanged: a form that
  could set `web.write` back to true is not a switch but a suggestion, and a form
  that could write `web.user` could lock this device or unlock it. The console and
  the file are the two ways in.

All of that is `test_web_the_gate_cannot_be_reached_from_a_network` and
`test_web_what_the_light_says_cannot_be_reached_from_a_network` rather than prose.

**And one mapping moved.** The three words a page sends for the radio —
`off`, `client`, `ap` — reach the two config fields through
`web::WifiWordFrom` / `web::WifiWordName` rather than through `ui::WifiMode`. That
enum exists next door because it is what a Wi-Fi *screen* cycles through, and
there is no screen here to own it; the mapping now has exactly one owner, is
declared in a header rather than hidden in a `.cpp`, and round-trips under test.

Everything else about the write path is the sibling's and inherited as decisions
rather than re-argued: a password can be written and never read back (absent means
keep it, matched by SSID; empty means clear it), an **absent `ip` block keeps the
address that is there** — the bug that board found by watching a `192.168.1.42`
turn into nothing — memory only, and the bus is reconnected while the radio is not.

### A password on the door

HTTP basic authentication over the whole site, off until `config.json` carries a
user *and* a password. `web_auth.h` is the rule and its four sub-rules are
unchanged and untouched: the pair is the switch, the credential is **encoded and
never decoded**, every path that cannot answer fails closed, and the comparison
does not stop early. Twenty-one host tests came with them and all twenty-one pass
here.

Two things to say plainly:

* **it is not TLS.** Basic auth puts the password on the wire in base64, which is
  not encryption, and §10.3 already says who can see the packets on this LAN. What
  it buys is real and bounded: the site is no longer open to whoever finds the
  address. TLS stays the real fix, in `tasks.md` §2.5's company;
* **a lockout is worse on this board than next door.** The `WWW-Authenticate`
  header is what makes a browser ask for a credential at all, and if it were
  missing there would be no way to type a password into this device — the sibling
  at least has an address on its glass, and this one has a single light and a
  console. That is why `401.html` says where the two words are set.

`web login <user> <pw>` on the console sets them, `web login off` clears them, and
the readout says **`OPEN`** in those words when only one half is set, because half
a credential is an open site and an operator reaches that state in one typo.

### The numbers

Measured on this build, `idf.py size` and `idf.py size-components`, against the
figures [`build.md`](build.md) recorded before this component existed:

| | |
|---|---|
| the whole image | 1,284,987 → **1,341,563 bytes**, **+56,576** |
| the app slot | 51 % → **49 % free** of 2.5 MB |
| `libweb.a` | **13,482 bytes**, of which **3,517 is `.bss`** — the 512-byte chunk buffer, the 1 KB JSON document, the 1,281-byte body buffer, the scan table and the 156-byte header buffer. §10.14.1 as a column: nothing here allocates |
| `libesp_http_server.a` | **11,117 bytes**, new, and in-tree — not a new entry on root §1's list |
| `libcli.a` | 20,339 → **21,970**, +1,631 for `web`, `web login` and two readout rows |
| `libconfig.a` | 8,814 → **9,260**, +446 for the four `web` fields and their parse and write |
| `liblwip.a` | 108,305 → **112,103**, +3,798 — the listening-socket paths the linker could previously drop. Worth naming because it is the largest single share of the total that is *not* this component |
| the site, in SPIFFS | **41,893 bytes** in eight files — seven pages and one `app.js` — on a partition with ~9.8 MB free. The one part of this that is free |

### The running cost, measured on this board

§10.16's two founding questions are **what does it cost while it is up** and
**does stopping give every byte back**. The sibling board answered them on a C6
with no PSRAM; those numbers are not this board's and were not borrowed. Here they
are, taken over the LAN at `192.168.11.190`:

| | |
|---|---|
| the server, while up | **11,064 bytes** of internal heap (the start log reports 10,932, which is before the handler first touches its own stack). Of that, **8,192 is the task stack** — so the server *itself* is about 2.9 KB, and the rest is §10.14.1's number for the one stack in this firmware that is not ours to place |
| **twenty start/stop rounds** | **+0 bytes end to end**, spread **64 bytes**, and the only movement in the whole table is a single −64 / +64 pair in rounds 10–11. Seventeen of the twenty are flat to the byte at 151,319 free |
| a second run of twenty | −112 end to end, spread 140, flat at 151,391 from round 16 on. **A first-run settling, not a leak** — the same shape the sibling board recorded and gave a paragraph to, where its first stop reported 168 bytes short because an access point raised two seconds earlier was still settling |
| the server task's own stack | **3,980 bytes at its deepest**, of 8,192 — measured after `/api/devstatus`, which is the heaviest handler there is. That number is why the constant is 8,192 and not the 4,096 it was ported with; the section below is that story |
| internal heap at rest, server down | ~151,300 free, low-water 132,039 across the whole session |

**So there is no leak on esp32s3 either**, which was the open question: `httpd_stop`
on ESP-IDF v6.0.2 gives back the task, its stack and the socket database, twenty
times in a row, to the byte. A drift that grows with the rounds is a leak; a spread
with no direction is the allocator, and 64 bytes with one pair moving in both
directions is as clean an answer as this experiment produces.

### The bug the board found, and why no other tier could

**The 4 KB task stack this component was ported with rebooted the device**, and the
shape of that failure is the whole argument for tier 3 on this section.

`/api/devstatus` runs the console's own dump on the *server's* task — that is the
design, and §10.7's reason for it is that a second copy of fifteen readouts drifts
from the first. On the sibling board that dump is I²C reads and formatting, and it
left **1,756 bytes of 4,096**. On this board the same dump includes `key` and
`keys` (§10.18) — readouts the console gives *itself* **12 KB** for — and it left
**116 bytes**.

116 bytes is not a margin, and what earns this a section is the failure mode rather
than the number:

* **the dump succeeded.** 3,339 bytes, `200`, well-formed, every section present.
  Nothing in the response said anything was wrong;
* **the board rebooted several requests later.** A FreeRTOS stack overflow is caught
  by a canary checked at a **context switch**, not at the write that overran the
  guard — so the corruption and the panic are separated by however long that task
  keeps running. From the host, what was visible was a later `POST /api/settings`
  dropping its connection, and then connection-refused;
* **three attempts to reproduce it from the request that appeared to trigger it all
  passed** — twice with that exact document, once with the whole six-request
  sequence on one keep-alive connection. Which is precisely what delayed detection
  looks like from outside, and is why the diagnosis had to come from
  `Status::task_stack_low` rather than from a repro.

The fix is one constant: **8,192**, a little over 2× the measured peak. It is not
12 KB like the console because the one thing that makes *that* number necessary is
unreachable from here — `key selftest` runs two curve operations and nothing on this
server can ask for one (§10.10 rule 4). `web` prints the margin against the
constant, and it is the number to re-check after any change to a readout.

**And no tier but the board could have found it.** The host tier compiles
`web_paths.cpp`, `web_auth.cpp` and `web_settings.cpp` — the three files that hold
rules — and `web_server.cpp` is not in that build at all: it is the socket, the task
and the `stdout` cookie. There is no stack in a host test.

### What is verified, and what is not

Stated here rather than left implied, because a ported component that builds is not
a ported component that works. All of the below was done over the LAN with the board
on this desk, at `192.168.11.190`, on an `app-flash` build:

* **`/api/status`** — the full document, with every field this board added and none
  it removed: `psram_free: 8380852`, `key_present: true`, `key_enrolled: true`,
  `light: "ready"`, `declined: 0`, and no `battery`, `battery_mv`, `charging` or
  `usb` anywhere in it;
* **`/api/settings`** — the same document the POST takes, with `secured: true` on the
  network and on the access point and **no passphrase anywhere in it**;
* **`/api/devstatus`** — 3,309–3,353 bytes, all nine sections, `text/plain`. So the
  `funopen` cookie and the global-`stdout` swap behave on this build's libc, which
  was inherited unchecked and is a row §10.11 had flagged;
* **the whitelist** — `/config.json`, `/fido.json`, `/registration.json`,
  `/../config.json` and a name that simply is not there: all **404 and
  indistinguishable**. `/` is a 404 too, for the reason at the end of this list;
* **the write path** — `{"approval":{"touchTimeoutSeconds":0}}` and
  `{"led":{"brightness":0}}` both refused **by name**, which is §10.10 rule 4 on the
  wire rather than in a test; and so are `{"web":{"user":"attacker"}}`, a `ws://`
  URL, `"mode":"yes"` and a fifth network — each with its own words and the field
  named. A real `{"nats":{"url":…}}` went through;
* **the actions** — `do=reconnect` and `do=save` returned `ok`; `do=nonsense`
  returned **400 `no such action`**, which is the useful shape: through the gate and
  refused by the verb;
* **the reboot guard** — `POST /api/reboot` with no query and with `?confirm=reboots`
  both **400 `not confirmed`** (the near-miss a `strstr` would have accepted), and
  `GET /api/reboot?confirm=reboot` a **404**, because there is no `GET` handler for
  it to reach;
* **the credential** — with `web login admin hunter2` set, eight ways of being wrong
  all answered 401 carrying
  `Basic realm="approver-esp32-yubikey", charset="UTF-8"`: no header, wrong
  password, wrong user, a prefix, a suffix, `Bearer` with the right bytes, and the
  bytes with no scheme — while a lower-case `basic` with extra spaces answered
  **200**, as designed. **`POST /api/reboot?confirm=reboot` with no credential
  answered 401 instead of restarting**, which is the one of these that would have
  been visible had the gate been one statement lower;
* **the realm is this board's own** — `approver-esp32-yubikey`, not the sibling's, so
  a browser holding both credentials can tell the two devices apart;
* **nothing was damaged by any of it** — `config` afterwards still shows the real
  network with its passphrase, the bus URL and `web auto`, and the uptime was
  unbroken across the whole sequence.

**What has not happened is a page.** The seven HTML files and `app.js` live in the
SPIFFS image, so serving one needs a full `idf.py flash` — which erases `fido.json`
and therefore costs a `key enrol` with the security key in hand (§10.18.1). That is
a decision with a physical price and it has not been taken; it is why `/` answers
404 above, and why every assertion in this section is an API endpoint or a refusal.
The markup itself is covered from the other side: `tests/test_esp32_web_pages.py`
checks each of this board's pages against the `app.js` handler that paints it, and
the id check is the one that catches a port.
