# approver-esp32 — the dependencies, the build, and what each of them cost

This file owns **§10.4** and **§10.12** of the project docs: every ESP-IDF
component this firmware depends on and the argument that got it signed off, the
ESP-IDF version question, the partition table, and the flash and RAM numbers
taken with `idf.py size-components` rather than estimated. Section numbers are
global and stable ([`../CLAUDE.md`](../CLAUDE.md) §2), so both keep their numbers
here.

Two documents sit either side of it. Root [`../CLAUDE.md`](../CLAUDE.md) §1 is the
**dependency allowlist** — the rule that nothing arrives without sign-off, and the
list of what has it; this file is where each entry on the C++ half of that list is
argued. And [`working-with-code.md`](working-with-code.md) has the **commands** —
where ESP-IDF is installed on this machine, how to get a shell in which `idf.py`
exists, how to flash, and how to run the host test suite; none of that is a
decision, which is why it is not here.

Where the rest of §10 lives:

- [`CLAUDE.md`](CLAUDE.md) — what this device is, its status, and the map;
- [`hardware.md`](hardware.md) — the board these components drive;
- [`firmware.md`](firmware.md) and [`protocol.md`](protocol.md) — the code that
  links against them;
- [`screens.md`](screens.md) — the screens LVGL draws;
- [`web.md`](web.md) — the configuration site and the 16 KB it took off LVGL;
- [`tests.md`](tests.md) — the host suite, built by MSVC rather than by ESP-IDF.

### 10.4 Dependencies — the list, and what was signed off

Root §1 requires explicit sign-off for anything outside the approved list.
**The C side has it now**: LVGL + `esp_lvgl_port`, the CO5300/CST9220 drivers,
libsodium for Ed25519, and `debsahu/espidf-nats` for the bus — approved by the
repository owner as argued below, and recorded in root §1. The alternatives stay
in the tables because they are the fallbacks if one of these turns out not to
work, not because the choice is still open.

**ESP-IDF v5.5.x** — the version line Waveshare builds this board's examples
against; other versions are documented as possibly incompatible with the
display/touch drivers. The exact number is the vendor's to state, and it has
moved: the published examples now live in
[`02_Example/ESP-IDF-v5.5.3`](https://github.com/waveshareteam/ESP32-C6-Touch-AMOLED-2.16/tree/main/02_Example/ESP-IDF-v5.5.3),
so **v5.5.3** is what a pin should say, not the v5.5.2 this section carried
before. **What is installed on this machine is v6.0.2** — §10.12 is where that
conflict got settled, and [`working-with-code.md`](working-with-code.md) has the
paths.

**And it was settled against a pin, so the manifest deliberately does not carry
one.** `main/idf_component.yml` says `idf: ">=5.5"` — a floor, not the pin this
paragraph originally asked for — because §10.12 found that the version-sensitive
half builds on v6.0.2 and that nothing left in this project has a claim on
v5.5.3. What v5.5.3 still is: the version the vendor's examples are written
against, and therefore the one to reach for if a display or touch problem ever
looks like a framework problem. What holds the *rest* of the versions is
`dependencies.lock`, committed, and that is where the `uv.lock` comparison
actually lands.

In-tree, arriving with ESP-IDF itself and needing no new supply chain:

| Component | For |
|-----------|-----|
| `esp_wifi`, `lwip` | the network, BSD sockets |
| `mbedtls` | base64 (`mbedtls_base64_*`), and TLS on the socket if §10.3 goes that way |
| `json` (cJSON) — **not in-tree any more**, see below | parsing requests, building replies — and the config files of §10.15 |
| `spiffs` | the `storage` partition: `config.json`, `config.init.json`, `registration.json` (§10.15) |
| `nvs_flash` | **nothing of ours** (§10.15) — initialised because `esp_wifi` needs it for calibration and PHY data |
| `esp_hmac`, `efuse` | the key custody of §10.6 |
| `esp_console`, `esp_vfs_usb_serial_jtag` | the provisioning commands of §10.7 |
| `esp_http_server` | the configuration web server of §10.16 — **in-tree, so not a new entry on root §1's list**, which is why §10.8.6 named it as the cheap way round a 6 mm keyboard. 46 KB of flash when it is linked, measured either side |
| `esp_lcd`, `driver` (I²C/SPI), `esp_timer`, `esp_event` | display transport, buses, the clock |

**cJSON left the framework, and that is the first thing v6.0.2 has actually
cost.** On the v5.5.3 this section pins it is the in-tree `json` component; on
the installed v6.0.2 it does not exist at all, and building against it means
`espressif/cjson` from the registry — which is what `main/idf_component.yml`
now declares (resolved: **1.7.19~2**, locked in the committed
`dependencies.lock`). The library is unchanged and was already signed off; only
its delivery is new. Two consequences worth recording: this is the project's
first managed dependency and therefore the first `dependencies.lock`, and it is
one more entry on the version ledger of §10.12 — on v5.5.3 the line would not
exist.

**And one host-side tool joined, which is not a dependency of anything.**
`ffmpeg` converts the sounds to the uncompressed WAV the firmware plays
(§10.8.1); it runs on this machine, nothing links against it, and the command
is in [`working-with-code.md`](working-with-code.md). The firmware deliberately
has **no decoder** — the argument is in `components/audio/speaker.h`.

New dependencies, all four approved:

| Approved | Why | The alternative it beat |
|----------|-----|-------------------------|
| `lvgl/lvgl` (v9) + `espressif/esp_lvgl_port` | seven screens, a scrolling list of scan results and an on-screen keyboard (§10.8) — hand-drawing that against `esp_lcd` primitives is weeks of work to reach something worse | draw directly against `esp_lcd`: defensible for the request card alone, not for the Wi-Fi screen, which is where the keyboard lives |
| CO5300 panel driver + CST9220 touch driver | the two chips on this board; the vendor demo carries both | none — they are the hardware. Prefer an Espressif-registry component over a copied vendor tree; if the vendor's is the only one, vendor it into `components/` with its version recorded |
| **an Ed25519 implementation** — `libsodium` (Espressif publishes it as a managed component) | mbedTLS has **no EdDSA**: it cannot sign or verify Ed25519 at all. §6's server key is Ed25519 *by fixed protocol* (`protocol.SERVER_KEY_TYPE`), so verify is not optional | switch the device's own key to `key_type: p256` (mbedTLS ECDSA, already a first-class scheme in §7 — the YubiKey and the browser both use it) — **but the registration reply still needs Ed25519 verify**, so this removes signing from libsodium's job, not libsodium |
| **a NATS client** — `debsahu/espidf-nats` (ESP Component Registry, `^1.4.0`, MIT, header-only C++, ESP-IDF 4.4–6.0) | the §10.5 subset without writing and debugging a socket state machine; and it brings TLS 1.2/1.3 with server-cert validation, mTLS and SNI, which is exactly what §10.3 needs and the one part of a hand-written client that would *not* have been ~300 lines | writing it ourselves, and the two options below |

**Two of those four names do not exist, and what they resolved to is the record
that matters.** The approval stands — these are the same components in intent —
but a table that keeps naming things that cannot be installed is a table nobody
can act on:

| §10.4 said | It is | Why |
|---|---|---|
| "CO5300 panel driver" | **`espressif/esp_lcd_sh8601` 2.0.1~1** | there is no CO5300 component on the registry, and the vendor's own ESP-IDF example drives this board with the SH8601 driver. The two share the QSPI command set, and everything specific to this glass arrives as the init list in `components/display/panel.cpp` — copied from that example, because a panel init sequence is not something to derive from a datasheet and hope |
| "CST9220 touch driver" | **`waveshare/esp_lcd_touch_cst9217` 1.0.4** | the part number in the board's documentation is CST9220 and the driver Waveshare publishes for it is `cst9217`. Not a discrepancy to resolve — the driver prints `Chip Type: 0x9220` when it probes this board, so it knows perfectly well what it is talking to. It brings `espressif/esp_lcd_touch` with it, which is the interface `esp_lvgl_port` also speaks |

And one the vendor took differently, where §10.4's choice was kept: Waveshare's
example is built on **`espressif/esp_lvgl_adapter`**, which drags in `button`,
`knob`, `freetype`, `esp_lv_decoder` and `esp_lv_fs` — five transitive
components for a device with three buttons it polls itself and no fonts to load
at runtime. Root §1 asks about exactly that weight. `esp_lvgl_port` **2.9.0**
depends on nothing but LVGL, covers the same ground, and is what §10.4 approved.

**LVGL is 9.4.0**, which is not 9.5 — §10.12.1's rule that the host preview and
the firmware must be the same minor version is now a live constraint rather
than a note: the simulator ships 9.5. Note where that constraint is actually
held: `main/idf_component.yml` says `^9.3.0`, which a fresh resolve would float
to 9.5 the moment one is published; what pins 9.4.0 today is `dependencies.lock`.
That is the `uv.lock` model and it is the intended one, but it means a
`idf.py update-dependencies` is a decision about the preview as well as about the
firmware.

**Two transitive components arrive that this table has not named**, and root §1
asks about exactly those. Both are on the display side and neither is a question
of weight:

| Transitive | Version | From | What it is |
|---|---|---|---|
| `espressif/esp_lcd_touch` | 1.2.1 | `waveshare/esp_lcd_touch_cst9217` | the common touch interface, named in the row above; this is the version it resolved to, and 1,437 bytes of flash next to the driver's own 2,343 |
| `espressif/cmake_utilities` | 0.5.3 | `espressif/esp_lcd_sh8601` | build-system helpers — CMake functions the panel driver's own `CMakeLists` calls. **No code of it reaches the firmware**: `idf.py size-components` has no line for it, the same way it has none for the WebSocket client, and for a stronger reason — there is nothing in it to link |

The third transitive component is `espressif/esp_websocket_client`, which is a
different kind of answer and has the section below to itself.

**And the whole display half resolved and built on ESP-IDF v6.0.2**, which is
the first real evidence in the v5.5.3-versus-v6.0.2 argument of §10.12 — see
there for what it does and does not settle.

**libsodium resolved to `espressif/libsodium` 1.0.22~1, and it is the cheapest
entry on this list to have signed off.** It brings **no transitive component at
all** — its manifest names `idf >=4.2` and nothing else — which is worth stating
next to the NATS client below, where the same question got the opposite answer.
Two things it did cost, both measured rather than assumed:

- **It is not in-tree here, which is cJSON's story a second time.** §10.4
  approved it as a managed component, and on the v5.5.x this section pins it is
  *also* an in-tree `libsodium`; on the installed v6.0.2 the in-tree copy is
  gone and the registry one is the only one. Same library, same sign-off, new
  delivery — and one more line on the version ledger of §10.12.
- **134,225 bytes when something references it, and nothing does yet.** With the
  §10.6 call surface linked — `sodium_init`, `crypto_sign_seed_keypair`,
  `crypto_sign_detached`, `crypto_sign_verify_detached` — `idf.py
  size-components` puts `libespressif__libsodium.a` at 134,225 bytes (133,022 of
  flash, 1,203 of DIRAM) and the app at 1,780,864 against the 2.5 MB slot. With
  the probe removed it has no line at all, exactly like the WebSocket client:
  built, never linked, and the app back at 1,646,640. So the number above is
  what §10.6 will spend, banked now rather than discovered then.

The Kconfig that goes with it is `CONFIG_LIBSODIUM_USE_MBEDTLS_SHA`, and §10.6
is where the argument for checking it lives; `sdkconfig.defaults` carries the
answer and what it saves.

**The NATS client is the interesting decision.** For Rust, §9.4 concluded that
reimplementing the protocol to avoid `async-nats` "would be far worse than
depending on it" — because an official client existed. For ESP-IDF there is no
official one, and the choice went to a third-party component anyway:

| Option | Assessment |
|--------|------------|
| **`debsahu/espidf-nats`** ← **chosen** | Header-only C++ on the registry, MIT, `idf.py add-dependency "debsahu/espidf-nats^1.4.0"`. Two hard requirements checked before choosing: `sub_internal` takes a `queue` argument, so the `approvers` group of §6 is expressible, and the message type carries `reply` — the reply-to subject §7 answers into. TLS and exponential-backoff reconnect come with it |
| Write it: ~300 lines over a socket | The subset is genuinely tiny (§10.5) and every failure mode would have been ours to shape. Rejected: the tiny part is the plaintext path, and §10.3's TLS is the part that is not tiny. Stays the fallback if the component becomes a problem — §10.5 remains a complete specification of what to write |
| `daed/nats-client` | The earlier port of `arduino-nats` the chosen one descends from; smaller, less maintained |
| `nats.io`'s official C client (`nats.c`) | Written for POSIX servers; pulls threads and an event loop and assumes desktop-sized memory. Not a realistic target here |

**Resolved: 1.4.0, and it brought something nobody signed off.** The component
is on the registry as described and the two hard requirements checked before
choosing it are true of the code as well as of the docs — `subscribe` takes a
queue group and `nats_msg_t` carries `reply`. What was not visible from the
registry page is its manifest: it requires **`espressif/esp_websocket_client`
unconditionally** (resolved 1.8.0, in `dependencies.lock`), which is exactly
the transitive component root §1 says is a new question for the owner. It is
raised here rather than buried:

- **nothing in this firmware can reach the WebSocket transport.** `nats_bus.h`
  is the whole call surface and it has no way to name a transport, and
  `endpoint.h` refuses `ws://` and `wss://` outright so that a URL cannot ask
  for one either;
- **it costs nothing in flash.** `idf.py size-components` has no line for
  `libespressif__esp_websocket_client.a` at all — the archive is built and
  never linked, because no symbol in it is referenced. The whole `nats`
  component, the header-only client instantiated inside it included, is
  **36,438 bytes** — 25,773 of them flash text and most of the rest the task's
  8 KB stack (§10.5 says what bought that);
- **it is compiled out, and the way that has to be done is a contradiction
  inside the dependency.** Because the component is always present, its
  CMakeLists puts `-DCONFIG_ESP_WEBSOCKET_CLIENT_ENABLE=1` on the command line
  of everything that includes it — but the matching `REQUIRES` never reaches
  ESP-IDF's requirement pass (it is computed from `BUILD_COMPONENTS`, which is
  not populated when that pass runs), so the *include directory* does not
  propagate. A define saying the transport is there and an include path saying
  it is not: the build fails on `#include <esp_websocket_client.h>`.
  `nats_bus.cpp` `#undef`s the macro before including the library, which lets
  the header's own `__has_include` fallback decide correctly. §10.4's "Kconfig-
  off what can be turned off" is that line.

**What the component brings that this device must never use.** JetStream,
KV, object store, NKey/JWT auth, WebSocket transport and message headers are all
in it; the responder uses `connect`, `subscribe` (with a queue group),
`publish`, `flush` and nothing else. The rule is §10.13's, applied to a
dependency: on the board, unused is not the same as absent — Kconfig-off what
can be turned off, keep `CONFIG_ESP_WEBSOCKET_CLIENT_ENABLE` unset, and record
what it costs with `idf.py size-components` (§10.12) rather than assuming it is
free. **It is C++**, which §10.14.1 turns from a problem into a non-issue — the
firmware is C++ as well, so it is included directly; the only wrapper around it
is the §10.5 contract, and that one exists to keep the call surface small, not
to cross a language boundary.

The rest of root §1's rule now applies to these four: each goes in
`main/idf_component.yml` with a version, they are locked in `dependencies.lock`
(committed), and **the exact versions land in these tables when the first build
resolves them** — "LVGL v9" is the approval, `9.4.0` is the record. Anything
beyond this list is a new question for the owner, including a transitive
component one of them drags in.

### 10.12 Build and flash (Windows)

**The commands are in [`working-with-code.md`](working-with-code.md)** —
`set-target` / `build` / `flash monitor`, and before them the thing that is not
obvious: `idf.py` does not exist until a shell is activated, and on this machine
that is a PowerShell profile rather than the `.cmd` this section once implied
(`idf.py` is a PowerShell *function*, so no child process and no `cmd.exe` sees
it). That is machine-local mechanics — the same kind of thing root §5 records
for the `py` launcher, given its own file because there is a page of it and none
of it is a decision. What belongs here is what the commands cannot say.

The first of those is a decision waiting to be taken:

**The installed version is v6.0.2, and §10.4 pins v5.5.3.** That is an open
conflict, not an oversight: v5.5.3 is what Waveshare's published examples are
built against (display and touch drivers are the parts documented as version-
sensitive), and `debsahu/espidf-nats` advertises 4.4–6.0, which v6.0.2 is
already past the top of. `eim` installs versions side by side and `eim select`
switches between them, so the resolution is a decision — install v5.5.3
alongside and build against it, or re-argue the pin against v6.0.x with the
vendor drivers in hand — to be taken at the first real build, and recorded in
§10.4 when it is.

**The version-sensitive half is now built, and it built on v6.0.2.** The
panel, the touch and LVGL — the exact three things this paragraph said would
decide it — resolve, compile and run on the installed version: `esp_lcd_sh8601`
2.0.1 declares `idf >=5.3`, `esp_lvgl_port` 2.9.0 declares `>=5.2`, and the
CST9217 driver `>=4.4.2`. So **v5.5.3 is no longer worth installing for this
reason**, and the pin in §10.4 should be read as "the version the vendor's
examples are written against" rather than as a requirement.

Two things it cost, both recorded rather than worked around: the `dc_gpio_num`
and SPI-host fields of the `esp_lcd` config are stricter about types in v6 than
the v5.5.3 example is written for, and `esp_lcd_touch_get_coordinates` is
deprecated there in favour of `esp_lcd_touch_get_data`. Neither is a reason to
go back a version; both are the reason a copied vendor file does not compile
unchanged.

**And so did the bus half.** `debsahu/espidf-nats` 1.4.0 advertises ESP-IDF
4.4–6.0 and was therefore the remaining candidate to reopen the version
argument; it resolves, compiles and connects on v6.0.2. What it needed was one
`#undef` for a WebSocket contradiction that is the component's own rather than
the version's (§10.4). So libsodium was the last unmeasured entry on §10.4's
list, and nothing left in this project has a claim on v5.5.3.

**And libsodium closed it, on the same version.** `espressif/libsodium` 1.0.22~1
compiles for RISC-V, links, and produces a signature that matches `lib/crypto.py`
byte for byte on this chip — §10.6 has what that settled and §10.4 what it
costs. **Every entry on §10.4's list is now resolved and measured**, which is
the thing this section has been asking for since the first build.

The numbers §10.12 asks for, taken with `idf.py size-components`: the whole
`nats` component, header-only client included, is **36,438 bytes** (25,773 of
them flash text, and 10,665 of DIRAM that is mostly the task's 8 KB stack), and
`espressif__esp_websocket_client` has no line at all — built, never linked. The
app is **1,646,640 bytes** against a 2.5 MB slot.

**`espressif__libsodium` has left that second category**, which is what §10.6
arriving means: **134,901 bytes** (133,694 of flash, 1,207 of DIRAM) with the key
custody linked against it, and the app at **1,788,928** of the 2.5 MB slot.
Measured under `CONFIG_LIBSODIUM_USE_MBEDTLS_SHA=y`; `n` costs about 10 KB more,
and §10.6 says why that is the more interesting half of the comparison.

And the two components on top of it, which are the §10.14.2 split showing up in
the size report a second time: `libcrypto.a` — the eFuse route, the fallback, the
self-test and the base64 — is **2,650 bytes** (2,502 of flash, 148 of DIRAM, of
which 144 is the key material itself: 32 bytes of public key, 64 of private, and
the 45-character base64 nobody wants to recompute). `libprotocol.a` had **no line at all**
when that was written — §7's signing bytes were host-tested and referenced by
nothing — and §6 and §7 collected it: it is **7,600 bytes**, every one of them
flash and **none of them RAM**, which is what a component made of nothing but
field layouts looks like. Next to it `libregistration.a` is 3,747, `libcrypto.a`
2,732, and `libresponder.a` **25,091** — of which 20,180 is DIRAM, and almost all
of that is two things the design asked for out loud: a 12 KB task stack sized to
hold a signature, and two 2.3 KB slots for decisions waiting to be signed.

Worth reading as a group rather than as five numbers. **Everything that decides —
the identity, the exchange, the wire format — is 14,079 bytes of flash and 148 of
RAM.** What costs is *holding* things: the responder's stack and slots, and
`libscreens.a`'s 23,942 of DIRAM for the card queue. And underneath all of it,
134,901 for the library that does one primitive. The protocol was never the
expensive part; the buffers §10.8.4 refuses to truncate are.

**And the last of the screens is on now**, which closes that comparison:
`ui/wifi_view.cpp`, `screens/wifi_screen.cpp` and `screens/wifi_scan_screen.cpp`
are **7,551 bytes of flash** between them, and the static RAM they bring is
**5,625** — of which **3,072 is one task stack**, sized for a blocking
`wifimgr::Scan` and then *measured* rather than left at the 5,120 it was guessed
at (§10.8.6 has both numbers). `libui.a` is **8,560 bytes**, still all flash, and
`libscreens.a` **66,435** (31,191 of DIRAM). The app is **1,861,552 bytes**
against the 2.5 MB slot.

**And the activity line of §9.10 is the smallest thing here yet measured**, which
is what a decision-holding layer is supposed to cost. Taken with `idf.py
size-components` after it: `libui.a` **9,132** (all flash — the view, the headline
and the two word tables), `libprotocol.a` **9,986** (3,084 of DIRAM, and the parser
next to §6's and §7's), `libscreens.a` **68,974** (31,708 of DIRAM — the label, its
buffer, and the layout that moved to make room), `libwatcher.a` **1,603** for two
subscriptions where it held one, and `libcli.a` **37,467**. The app is **1,925,072
bytes**, 27 % of the slot still free. The lines above are the previous measurement
of the same three archives rather than a like-for-like baseline — three unrelated
commits sit between them — so the number to read here is the shape, not a
subtraction: one line of glass, one parser, one enum pair, and the whole of it is
kilobytes.

**A later rule on that same line moved two of those numbers**, and they are worth
quoting for how little they moved. Only a *running* tool is named now — `thinking`
and `idle` are their own word (§10.8.3) — which is one condition inside `Headline`:
`libui.a` **9,134**, two bytes more. The console gained the row that prints the
tool the glass no longer names: `libcli.a` **37,543**, seventy-six more. The app is
**1,925,168 bytes**, the same 27 % of the slot free. A behaviour change that costs
under a hundred bytes is what the split between a view that decides and a screen
that draws is for.

**And a deletion, measured the same way, for the shape rather than the saving.**
The settings list's placeholder mechanism went — a `kNotBuilt` action, a `Built()`
predicate and the faint `soon` note the screen drew with them, all of it carrying
no row since §10.8.6 gave the last one a screen (`ui/settings_menu.h` says why it
is a note now instead of code). `libui.a` **9,088** (46 fewer), `libscreens.a`
**68,894** (80 fewer), the app **1,924,992 bytes** — **176 bytes** for a mechanism,
its drawing and its log line, which is the same point from the other end: a
decision layer this cheap is a layer nobody has to ration.

Read next to the paragraph below, it is the same shape a third time: the layer
with every decision in it costs a kilobyte of flash and no RAM at all, and what
costs is *holding* things — a task stack, sixteen SSIDs, and the label buffers
that keep §10.8.4's promise never to truncate anything.

And the two screens of §10.8.2 and §10.8.4, because they are the first data point
for what three more of them cost: `libscreens.a` is **30,372 bytes** (10,770 of
flash and 19,602 of DIRAM) and `libui.a` — the navigator, the whole of the clock
face's arithmetic including its sine table, and the request card's queue *logic* —
is **2,648 bytes**, all of it flash.

Two things in that pair are worth reading rather than skimming. The **ratio** is
the §10.14.2 split showing up in the size report: the half with every decision in
it is the cheap half, twelve times over. And `libscreens.a`'s DIRAM is where the
card actually costs something — 16,990 of `.bss` and 2,612 of `.data` against the
clock's 4,432 — because a queue of four requests with §7's fields in them is about
9 KB, the request handed back to a verdict is another 2.3, and the screen's label
buffers are 2.3 more. That is the price of §10.8.4's "never truncated": the
buffers are big enough that a real command fits, and a payload that does not fit
is refused rather than cut.

**These figures are a snapshot and drift with every commit**, which is worth
saying once rather than re-taking them silently: the three recorded before this
pass (`nats` 32,218, `libwifi.a` 5,098, `libwifimgr.a` 7,379) were all short by
the time anybody read them back, and the one that had barely moved was the app
total. The point of recording them is the *shape* — a header-only client that
costs 25 KB of text and no WebSocket transport at all, a radio whose flash cost
is the framework's and not ours — and a number that disagrees with this
paragraph by a few per cent is the paragraph being old, not a regression.

The targets this install has support for are `esp32`,
`esp32c6`, `esp32p4` and `esp32s3`, so `set-target esp32c6` is not the blocker.

The project those commands run in is **generated** — `create-project` plus a
`create-component` per piece of the library layer, not a copy of another board's
tree (§10.14.4 says what is taken from the house firmware instead).

Notes that will otherwise be rediscovered the hard way:

- **The port is the USB Serial/JTAG device**, and it is the same port
  `esp_console` serves the §10.7 commands on — the monitor and a manual `register`
  cannot both hold it.
- **16 MB of flash, and a partition table that says so.** The default table
  assumes 2 MB or 4 MB; LVGL plus Wi-Fi plus TLS will not fit it. `partitions.csv`
  is the custom one and it is **written**: `nvs`, `nvs_keys` (reserved and empty,
  §10.15), `otadata`, two 2.5 MB OTA slots and ~10.9 MB of `storage`, which
  carries the SPIFFS image built from `spiffs_image/`. Changing an offset after a
  device is registered means reflashing it, which is why the table was settled
  before any of it was needed.
- **Flash encryption and Secure Boot are what make §10.6's fallback meaningful**
  and they are one-way operations on real hardware. Read Espressif's docs
  before burning anything, and keep a development board that has not been
  through it.
- `idf.py size-components` is how you find out what the dependency decisions of
  §10.4 actually cost; record the numbers when they are taken.

#### 10.12.1 The LVGL preview — screens before there is a board

`lvgl-mcp-server` ([jaklys/Lvgl-mcp-esp32](https://github.com/jaklys/Lvgl-mcp-esp32),
npm, v2.0.0) is an MCP server that compiles an LVGL snippet against a headless
desktop build of LVGL and hands back a PNG plus a JSON widget tree —
`lvgl_render`, `lvgl_render_full`, `lvgl_inspect`, `lvgl_set_resolution`. It is
how the seven screens of §10.8 get drawn and argued about while §10.13's build
order still has the request card at step 1. **A host-side design tool, not a
dependency**: nothing in `main/` links against it, and it does not enter §10.4's
list.

It is registered project-wide in [`../.mcp.json`](../.mcp.json) — project scope,
so Claude Code asks to approve it once per machine (`/mcp`). Three entries there
are deliberate and will look arbitrary later — a global install rather than
`npx`, a hand-set `VCVARSALL_PATH`, a borrowed ninja — and the reason for each,
along with the silent postinstall failure that leaves the package without its
simulator, is in [`working-with-code.md`](working-with-code.md). None of it is a
decision about the firmware; all of it is why a render fails on this box.

Two rules about believing what it shows:

- **Set 480×480 first** (`lvgl_set_resolution`, or the per-call `width`/
  `height`) — the default is 800×480, and a layout designed at the wrong size is
  worse than no preview at all.
- **It proves the layout and nothing else.** The simulator has a desktop's RAM,
  no CO5300 and no CST9220: it says nothing about the partial buffers of §10.1,
  flush timing, the colour path or touch. A card that looks right here can still
  be too heavy for 512 KB. And the preview only stays honest while the
  simulator's LVGL and the firmware's are the same minor version (it ships 9.5)
  — when §10.4's LVGL gets pinned, pin it against this one.

#### 10.12.2 A screenshot of the real panel — the other half of that

The preview above renders a screen with no board. This is the opposite and it
closes the gap the preview cannot: **what the glass is actually showing**,
returned as a PNG. Between them, a layout can be argued about before it exists
and checked after it does.

The reason it is not simply `lv_snapshot_take` is §10.1 twice over: QSPI to the
CO5300 is **write-only**, so the panel cannot be read back, and a 480×480 frame
at 16 bpp is 460,800 bytes against 512 KB of SRAM shared with lwIP, Wi-Fi and
LVGL's own pool, with no PSRAM and no way to add it. There is nowhere to put a
frame, so the frame is never assembled.

**So it is streamed through the one place the pixels already pass in pieces.**
LVGL renders a partial buffer at a time; `display::Capture` hooks
`LV_EVENT_FLUSH_START`, which fires with the area about to be written and — via
`lv_display_get_buf_active` — the buffer it was rendered into. Each piece is
base64-encoded straight to the console and forgotten. The memory this costs is a
720-byte staging buffer and the line it encodes into.

Four things that are decisions rather than plumbing:

- **The hook works because of an ordering in `lv_refr.c`**, and that is worth
  writing down rather than rediscovering: LVGL swaps its two buffers *after* the
  flush callback returns, so during the event the active buffer and the one being
  flushed are the same one. And `flushing_last` is set *before* the callback, so
  `lv_display_flush_is_last` is what says the frame is complete — a fact to read
  rather than a row count to add up and get wrong.
- **The pixels are LVGL-native little-endian**, not the big-endian this glass
  wants: `esp_lvgl_port` swaps in place inside its own flush callback, which is
  after the event. Nothing to work around, and it is the order a host decoder
  wants anyway — but a byte order that depends on *where* in the path you look is
  exactly the thing to state once.
- **It stalls the display for as long as the transfer takes**, a few seconds,
  from inside the LVGL task. That is what §10.8.1 forbids and it is deliberate:
  somebody typed the command, and the alternative is a screenshot that does not
  exist. It shows up honestly in `clock`, whose count of frames given up for the
  display goes up by a handful each time.
- **Pieces carry their own rectangle**, so the decoder places them rather than
  concatenating. A full-screen invalidate does produce full-width bands in
  order, so concatenation would work today — and would break silently the first
  time it did not.

The host half is **`tools/screenshot.py`**, and it writes the PNG with `zlib`
and `struct` rather than an image library: root §1 keeps a short list and this
was not worth a conversation. `--from` decodes a capture taken any other way,
which is also how the decoder is tested with no board — a synthetic capture with
a known picture in it makes a shear, a byte-order slip and an upside-down frame
all visible.

**And the way to know the picture is the screen** rather than a plausible image:
`clock` next to it. It prints the digits, the drift and every indicator, so the
two are independent answers — and since the drift is what places the geometry, a
capture whose *pixels decode* to the time `clock` reported is a capture in the
right place as well. Done on this board: `15:50` off the console and `15:50` read
back out of the segment boxes of the PNG, at a drift of `+10,-8`.

[`working-with-code.md`](working-with-code.md) has the one command;
[`commands.md`](commands.md) has the wire format.

