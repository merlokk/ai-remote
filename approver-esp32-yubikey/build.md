# The dependencies and the build (§10.4, §10.12)

## 10.4 What this links against

Root [`../CLAUDE.md`](../CLAUDE.md) §1 requires sign-off for any dependency
outside its list. **This board's list is the sibling board's minus four and plus
one**, and the one addition is the only entry here that needed a new argument.

Declared in `main/idf_component.yml`, locked in `dependencies.lock` (committed) —
the same rule `uv.lock` and `Cargo.lock` follow for the other halves of this
repository.

| Component | Version | Why |
|-----------|---------|-----|
| `espressif/cjson` | `^1.7.18` | `config.json`, `fido.json`, and §7's wire format |
| `debsahu/espidf-nats` | `^1.4.0` | the bus (§10.3, §10.5) |
| `espressif/libsodium` | `^1.0.22` | Ed25519 — mbedTLS has none, and §6's server key is Ed25519 by fixed protocol |
| **`espressif/usb`** | **`^1.5.0`** | **the USB Host Library — new, and argued below** |

**Gone with the panel**: `lvgl/lvgl`, `espressif/esp_lvgl_port`,
`espressif/esp_lcd_sh8601` and `waveshare/esp_lcd_touch_cst9217` — four
components and one transitive one (`espressif/esp_lcd_touch`). That is most of
why this firmware is 1.31 MB where the sibling's is closer to 2.

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
  host-tests (§10.18.3). Taking the class driver would add a dependency in order
  to *not* use most of it. **One new component instead of two.**

It costs **35,322 bytes** of the image (`idf.py size-components`), of which 890
are IRAM.

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
CONFIG_ESP_CONSOLE_UART_DEFAULT=y     # NOT USB Serial/JTAG - §10.1, §10.18.3
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
Total image size: 1,309,451 bytes
Smallest app partition: 2,621,440 bytes — 50% free
Bootloader: 21,056 bytes — 36% free
```

The largest archives, and none of them is a surprise:

| Archive | Bytes | What |
|---------|-------|------|
| `libnet80211.a` | 150,645 | the Wi-Fi MAC |
| `libespressif__libsodium.a` | 135,955 | Ed25519 |
| `libesp_stdio.a` | 112,665 | mostly `.rodata` — `printf` and its tables |
| `libtfpsacrypto.a` | 111,161 | mbedTLS/PSA, linked for `esp-tls` and used by §10.18 |
| `liblwip.a` | 109,883 | TCP/IP |

And this project's own, which is the interesting half:

| Archive | Bytes | of which `.bss` |
|---------|-------|-----------------|
| `libresponder.a` | 50,772 | **36,142** — four 2.3 KB requests, two pending decisions, two task stacks |
| `libespressif__usb.a` | 35,322 | 17 |
| `libnats.a` | 33,016 | 10,496 |
| `libfido.a` | 27,838 | **12,420** — the 2 KB CTAPHID buffer, a 512-byte request buffer, the enrolment |
| `libcli.a` | 23,151 | 5,248 |
| `libconfig.a` | 9,358 | 5,230 |
| `libprotocol.a` | 8,699 | 0 |
| `libled.a` | 5,357 | 2,800 — one task stack |
| `libindicator.a` | 4,901 | 3,192 — one task stack |
| `libui.a` | 1,672 | 0 |
| `libbuttons.a` | 869 | 0 |
| `libboards.a` | 512 | 102 |

**The `.bss` column is where §10.14.1 shows up as a number.** Nothing here
allocates, so every buffer is in that column and every one of them is a decision
that was made once: `libresponder.a`'s 36 KB is four queued requests at 2.3 KB
each plus two decisions waiting to be signed, and `libfido.a`'s 12 KB is the
CTAPHID reassembly buffer capped at 2 KB rather than the protocol's 7,609.

`libboards.a` at 512 bytes is the whole of this board's hardware layer, which is
the clearest single number for what §10.13's list of absences bought.

### Flashing

`working-with-code.md` has the commands. The one thing worth repeating here
because it costs a registration:

**A full `idf.py flash` writes the SPIFFS image, which erases `config.json`,
`registration.json` and `fido.json`.** The Wi-Fi passphrase, the registration and
the key enrolment all go with it — and the registration cannot be recovered
without a **new** one-time token minted on the host, because the old one is spent.

The identity is *not* lost: the Ed25519 seed lives in NVS, which a `flash` does
not touch, so the device comes back with the same `key_id` and the same public key
— unregistered rather than unknown.

`idf.py app-flash` has none of these consequences and is what the edit-build-run
loop should use.
