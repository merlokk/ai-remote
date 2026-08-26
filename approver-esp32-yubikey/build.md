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

It costs **35,354 bytes** of the image (`idf.py size-components`), of which 890
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
Total image size: 1,298,215 bytes
Smallest app partition: 2,621,440 bytes — 50% free
Bootloader: 21,056 bytes — 36% free
```

The largest archives, and none of them is a surprise:

| Archive | Bytes | What |
|---------|-------|------|
| `libnet80211.a` | 150,629 | the Wi-Fi MAC |
| `libespressif__libsodium.a` | 135,955 | Ed25519, and now only to *verify* (§10.6) |
| `libtfpsacrypto.a` | 111,117 | mbedTLS/PSA, linked for `esp-tls` and used by §10.18 |
| `libesp_stdio.a` | 108,533 | mostly `.rodata` — `printf` and its tables |
| `liblwip.a` | 108,305 | TCP/IP |

And this project's own, which is the interesting half:

| Archive | Bytes | of which `.bss` |
|---------|-------|-----------------|
| `libresponder.a` | 50,850 | **36,142** — four 2.3 KB requests, two pending decisions, two task stacks |
| `libespressif__usb.a` | 35,354 | 17 |
| `libfido.a` | 34,732 | **14,804** — the 2 KB CTAPHID buffer, a 1,152-byte request buffer sized from §10.18's ceilings, the enrolment |
| `libnats.a` | 33,032 | 10,496 |
| `libcli.a` | 20,339 | 5,248 |
| `libwifimgr.a` | 8,968 | 4,499 — one task stack |
| `libconfig.a` | 8,814 | 5,066 |
| `libprotocol.a` | 6,633 | 0 |
| `libwifi.a` | 5,456 | 1,472 |
| `libled.a` | 5,333 | 2,660 — one task stack |
| `libindicator.a` | 4,873 | 3,192 — one task stack |
| `libarkg.a` | 3,921 | 0 — §10.18's derivation, and it allocates nothing at all |
| `libcrypto.a` | 2,310 | 144 |
| `libui.a` | 1,486 | 0 |
| `libbuttons.a` | 775 | 0 |
| `libboards.a` | 512 | 102 |

**The `.bss` column is where §10.14.1 shows up as a number.** Nothing here
allocates, so every buffer is in that column and every one of them is a decision
that was made once: `libresponder.a`'s 36 KB is four queued requests at 2.3 KB
each plus two decisions waiting to be signed, and `libfido.a`'s 14.8 KB is the
CTAPHID reassembly buffer capped at 2 KB rather than the protocol's 7,609, a
request buffer sized from the *ceilings* rather than from what a YubiKey happens to
send (`fido.cpp` shows the arithmetic — a tight one there would fail at the moment
of an approval and name nothing), and an enrolment that now carries a seed key and
a key handle (§10.18.1).

**What §10.18's change to the signer cost, in total: about 10 KB** — 3,921 for the
derivation, the rest in `fido` and `ctap2` for `previewSign` and the wider
enrolment. Nothing was added to the dependency list to get it.

`libboards.a` at 512 bytes is the whole of this board's hardware layer, which is
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
and needs `key enrol` before `register` will even run. The Ed25519 seed in NVS
survives a `flash` and is not what signs anything (§10.6).

`idf.py app-flash` has none of these consequences and is what the edit-build-run
loop should use.
