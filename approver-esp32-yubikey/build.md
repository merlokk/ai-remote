# The dependencies and the build (§10.4, §10.12)

## 10.4 What this links against

Root [`../CLAUDE.md`](../CLAUDE.md) §1 requires sign-off for any dependency
outside its list. **Four components, and one of them is the only entry here that
needed a new argument.**

Declared in `main/idf_component.yml`, locked in `dependencies.lock` (committed) —
the same rule `uv.lock` and `Cargo.lock` follow for the other halves of this
repository.

| Component | Version | Why |
|-----------|---------|-----|
| `espressif/cjson` | `^1.7.18` | `config.json`, `fido.json`, and §7's wire format |
| `debsahu/espidf-nats` | `^1.4.0` | the bus (§10.3, §10.5) |
| `espressif/libsodium` | `^1.0.22` | Ed25519 — for **verifying §6's reply** only; mbedTLS has no EdDSA and the server key is Ed25519 by fixed protocol. Since §10.18 nothing on this device *signs* with it |
| **`espressif/usb`** | **`^1.5.0`** | **the USB Host Library — new, and argued below** |

**There is no graphics library on this list and none is coming.** There is nothing
on this board a graphics library could draw on (§10.1) and nothing this firmware
has to say that one WS2812 cannot (§10.17), so its absence is a design decision
rather than a gap — which is most of why this firmware is 1.3 MB.

### The one new dependency

`espressif/usb` is the USB Host Library. Three things about it:

* **it is ESP-IDF's own, and it used to be in the tree.** On v5.5.x this is
  `components/usb` inside the framework. On the v6.0.2 installed here the in-tree
  copy is gone and the registry one is the only one — which is the **third**
  component on this list with that history, after cJSON and libsodium. It is
  Espressif's code either way, versioned with the framework;
* **it brings nothing transitive.** Its manifest names `idf` and nothing else,
  which is the opposite of what the NATS client does (see below);
* **the HID class driver is deliberately not taken.** `espressif/usb_host_hid`
  exists and would be the obvious second entry. It is the wrong tool: a FIDO key
  is not a boot-protocol keyboard, CTAPHID is its own framing on top of two raw
  interrupt endpoints, and that framing is ~200 lines this firmware owns and
  host-tests (§10.18.4). Taking the class driver would add a dependency in order
  to *not* use most of it. **One new component instead of two.**

It costs **35,366 bytes** of the image (`idf.py size-components`), of which 890
are IRAM.

### The dependency §10.18 did *not* add

The ARKG derivation (§10.18.2) needs an elliptic curve, and there is an obvious
way to get one wrong: add `micro-ecc`, or vendor a P-256 implementation, and put a
fresh crypto library on root §1's list.

**Neither happened.** `components/arkg` uses **PSA Crypto**, which is already in
this image for `esp-tls`, for the hash, the ECDH and the scalar multiplication; the
one operation PSA has no entry point for — adding two public points — is
`mbedtls_ecp_muladd`, reached through `mbedtls/ecp.h`, which on v6 is *ESP-IDF's
own shim* over the private header (§10.18.2 argues why that is a supported path and
not a reach around one).

So the dependency list does not move, and the derivation costs **3,921 bytes**:

```
libarkg.a    3,921    of which 3,420 .text, 501 .rodata, 0 .bss
```

Zero `.bss`, because everything it needs is on the caller's stack for the
milliseconds a derivation takes. `CONFIG_MBEDTLS_ECP_C` and
`CONFIG_MBEDTLS_ECP_DP_SECP256R1_ENABLED` were already on in the inherited
defaults — TLS needs them — so the curve arithmetic is bytes that were in the image
before this component existed.

### The dependency §10.16 did *not* add either

The configuration web server (§10.16) needs an HTTP server, a place to keep pages
and a base64 encoder for its credential, and there is an obvious way to get all
three wrong: add a server component, a template library, and a crypto library for
the encoder.

**None of that happened, and two of the three are worth a line each.**

`esp_http_server` is **in-tree** — it arrives with ESP-IDF, the same status cJSON
and libsodium had on v5.5.x — so it is not a new entry on root §1's list. It costs
**11,117 bytes**, and it is the whole of what was added to the dependency line:
`components/web`'s `REQUIRES` is otherwise components this firmware already had.

And the base64 encoder is **twenty lines of table lookup in `web_auth.cpp`**, not
mbedTLS and not libsodium. That is not thrift: the credential is *encoded and never
decoded* (`web_auth.h` argues why that direction is safer — a device with no decoder
has none of a decoder's parsing bugs), and keeping the encoder local is what lets
the one comparison standing between a network and this device's settings compile in
the host tier with a bare C++ compiler. Neither crypto library is on that
component's line, and neither is in the host build.

**The pages are files, not code.** Seven HTML documents and one `app.js`, 41,893
bytes on a partition with ~9.8 MB free, served off SPIFFS by name — no template
engine, no build step, no framework, for the same reason this firmware has no image
decoder.

### The transitive one nobody signed off in advance

`debsahu/espidf-nats` requires `espressif/esp_websocket_client` unconditionally,
even though its CMakeLists is written to build without one, and there is no way
to decline it from the manifest. Nothing in this firmware reaches the WebSocket
transport — `nats_bus.h` is the whole call surface and it has no way to name one
— so what it costs is a download and whatever the linker cannot garbage-collect.
Root §1 asks about exactly this kind of thing; it is recorded here for the same
reason the sibling folder records it.

### Host-side tools

| Tool | Where | What for |
|------|-------|----------|
| MSVC, CMake, Ninja | already on this machine | §10.11's host test tier |
| `pyserial` | ESP-IDF's own venv | driving the console from a script (`working-with-code.md`) |

None of them is in the build, and nothing new was installed to run a test.

## 10.12 The build

### The target and the memory

```
idf.py set-target esp32s3
```

`sdkconfig.defaults` carries what that has to be told, and every number in it was
read off the chip rather than off a product page (§10.1): 16 MB of quad flash,
8 MB of **octal** PSRAM. The two lines most worth knowing:

```
CONFIG_ESP_CONSOLE_UART_DEFAULT=y     # NOT USB Serial/JTAG - §10.1, §10.18.4
CONFIG_USB_HOST_HUBS_SUPPORTED=n      # a key plugs straight in
```

The first is the opposite of what the sibling board sets, and it is the single
line that makes this design possible: the chip's own USB has another job.

The second is a choice rather than a constraint. Hub support costs code and an
external-hub driver whose error paths Espressif's own documentation lists as
unimplemented, for a topology this device does not have.

### The partition table

`partitions.csv`, unchanged from the sibling board because the flash is the same
size: two 2.5 MB OTA slots and ~11 MB of storage.

```
nvs        0x9000    0x6000
phy_init   0xf000    0x1000
nvs_keys   0x10000   0x1000   encrypted
otadata    0x11000   0x2000
ota_0      0x20000   0x280000
ota_1      0x2a0000  0x280000
storage    0x520000  0xae0000
```

Offsets are written out rather than left to the generator: app partitions have to
be 64 KB aligned and an off-by-one there is a silent reflash away from confusing.

### The numbers

Measured on this build, `idf.py size` and `idf.py size-components`:

```
Total image size: 1,341,547 bytes
Smallest app partition: 2,621,440 bytes — 49% free
Bootloader: 21,056 bytes — 36% free
```

**Every number in this section was re-taken against the tree as it stands.** The
two tables below had drifted in a way worth naming, because it is the way a size
table always drifts: a change that adds a component updates the rows it is *about*
and leaves the rest, so `libweb.a` and `liblwip.a` were exact while `libbuttons.a`
still read 775 — from before the 10 ms poller of §10.18.5 — and `libcrypto.a` still
read 2,310, from before its identity was deleted. The total was right the whole
time, which is what let it go unnoticed.

**The last change to move that total by anything worth a table was §10.16's
configuration web server** — the sibling board's site, ported, measured at
**1,284,987 → 1,341,563, +56,576 bytes**. Where it went:

| | Bytes |
|---|---|
| `libweb.a` | **13,486**, of which **3,517 is `.bss`** — a 512-byte chunk buffer, a 1 KB JSON document, a 1,281-byte body buffer, the scan table and a 156-byte header buffer. §10.14.1 as a column again: nothing in it allocates |
| `libesp_http_server.a` | **11,117**, and **in-tree** — it arrives with ESP-IDF and is not a new entry on root §1's list, which is why §10.4 below did not have to grow |
| `liblwip.a` | 108,305 → **112,103**, **+3,798** — the listening-socket paths the linker could previously discard. The largest single share of the total that is not this component's own code |
| `libcli.a` | 20,339 → **21,951**, +1,612 for `web`, `web login` and two readout rows |
| `libconfig.a` | 8,814 → **9,260**, +446 for the four `web` fields, their parse and their write |
| the site itself | **41,893 bytes** of SPIFFS in eight files, on a partition with ~9.8 MB free. Not in the image at all |

The rest is `esp_netif`, socket and TLS-adjacent code the linker keeps once
something listens.

**And the running cost, which is not in the image at all**, measured on the board
over the LAN: **11,064 bytes** of internal heap while the server is up — of which
**8,192 is the task stack**, so the server itself is about 2.9 KB — and **+0 bytes
over twenty start/stop rounds**, spread 64. There is no leak on esp32s3.
[`web.md`](web.md) §10.16 has the table, the second run, and the stack overflow that
made that 8,192 a measured number rather than the 4,096 it was ported with.

**And 1,284,987 was itself 51 % free**, which was
**13,228 bytes smaller** than the version before it — and that change was a
deletion: §10.6's Ed25519 identity, the eFuse route through the HMAC unit, the seed
generated from SAR-ADC entropy and kept in NVS, `Sign`, and `ProveKey`. Three
ESP-IDF components left the dependency line with them — `esp_security`, `efuse` and
`bootloader_support` — which is most of where the bytes were. libsodium itself stays
for one verify.

The largest archives, and none of them is a surprise:

| Archive | Bytes | What |
|---------|-------|------|
| `libnet80211.a` | 150,637 | the Wi-Fi MAC |
| `libespressif__libsodium.a` | 124,149 | Ed25519, and now **only** to verify — one call, plus base64 (§10.6). Still the largest thing this firmware links for the smallest surface, and the reason is fixed protocol: §6's server key is Ed25519 and mbedTLS has no EdDSA. It was 135,955 while `Sign` existed; the ~11.8 KB the linker could then discard is the second-largest thing §10.6's deletion bought, after the private key itself |
| `libesp_stdio.a` | 121,911 | mostly `.rodata` — `printf` and its tables |
| `liblwip.a` | 112,103 | TCP/IP |
| `libtfpsacrypto.a` | 110,993 | mbedTLS/PSA, linked for `esp-tls` and used by §10.18 |

And this project's own, which is the interesting half:

| Archive | Bytes | of which `.bss` |
|---------|-------|-----------------|
| `libresponder.a` | 55,025 | **40,239** — four 2.3 KB requests, two pending decisions, two task stacks (one of them `kGateStackBytes`' 12 KB, raised after it was measured twice) |
| `libespressif__usb.a` | 35,366 | 17 |
| `libfido.a` | 35,018 | **14,804** — the 2 KB CTAPHID buffer, a 1,152-byte request buffer sized from §10.18's ceilings, the enrolment |
| `libnats.a` | 33,040 | 10,496 |
| `libcli.a` | 21,951 | 5,248 |
| `libweb.a` | 13,486 | 3,517 — §10.16's five buffers, and no more than five |
| `libconfig.a` | 9,260 | 5,166 |
| `libwifimgr.a` | 8,963 | 4,499 — one task stack |
| `libprotocol.a` | 6,625 | 0 |
| `libwifi.a` | 5,456 | 1,472 |
| `libled.a` | 5,381 | 2,660 — one task stack |
| `libindicator.a` | 5,012 | 3,192 — one task stack |
| `libarkg.a` | 3,921 | 0 — §10.18's derivation, and it allocates nothing at all |
| `libbuttons.a` | 2,623 | 1,636 — the 10 ms poller and its latch, which is what made a 70 ms tap on BOOT visible to a gate blocked inside a USB read (§10.18.5). It was 775 bytes and no `.bss` before that |
| `libui.a` | 1,489 | 0 |
| `libcrypto.a` | 927 | 2 — and it was 2,310 while there was a key to derive (§10.6) |
| `libboards.a` | 560 | 2 |

**The `.bss` column is where §10.14.1 shows up as a number.** Nothing here
allocates, so every buffer is in that column and every one of them is a decision
that was made once: `libresponder.a`'s 40 KB is four queued requests at 2.3 KB
each plus two decisions waiting to be signed, and `libfido.a`'s 14.8 KB is the
CTAPHID reassembly buffer capped at 2 KB rather than the protocol's 7,609, a
request buffer sized from the *ceilings* rather than from what a YubiKey happens to
send (`fido.cpp` shows the arithmetic — a tight one there would fail at the moment
of an approval and name nothing), and an enrolment that now carries a seed key and
a key handle (§10.18.1).

**What §10.18's change to the signer cost, in total: about 10 KB** — 3,921 for the
derivation, the rest in `fido` and `ctap2` for `previewSign` and the wider
enrolment. Nothing was added to the dependency list to get it.

`libboards.a` at 560 bytes is the whole of this board's hardware layer, which is
the clearest single number for what §10.13's list of absences bought.

### Flashing

`working-with-code.md` has the commands. The one thing worth repeating here
because it costs a registration:

**A full `idf.py flash` writes the SPIFFS image, which erases `config.json`,
`registration.json` and `fido.json`.** The Wi-Fi passphrase, the registration and
the key enrolment all go with it — and the registration cannot be recovered
without a **new** one-time token minted on the host, because the old one is spent.

**And since §10.18 the enrolment is the identity**, so losing `fido.json` is
losing the signing key: the device comes back with the same `key_id` and *no* key,
and needs `key enrol` before `register` will even run. There is no Ed25519 seed to
survive a `flash` any more: §10.6's identity is deleted and the firmware erases the
seed an older build left in NVS.

`idf.py app-flash` has none of these consequences and is what the edit-build-run
loop should use.
