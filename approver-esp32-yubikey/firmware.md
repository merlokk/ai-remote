# What the device does for itself (§10.9, §10.14, §10.15)

**None of which can approve anything.** As with
[`protocol.md`](protocol.md), most of this is the sibling board's unchanged, and
[`../approver-esp32/firmware.md`](../approver-esp32/firmware.md) is the long form
of §10.9, §10.14 and §10.15. What follows is what differs.

## 10.9 Wi-Fi

Identical. `components/wifi` (the radio) and `components/wifimgr` (the policy
above it) are copied byte for byte, including the split that makes
`wifi_policy.cpp` and `reachability.cpp` testable with no board.

The two things worth restating because they are visible on this device's light:

* **`active` is one switch, not two.** `wifi.active` false is off whatever the
  mode says. `esp_wifi_init` costs tens of kilobytes of heap and a device
  configured with Wi-Fi off should not pay them, which is why `wifimgr::Init`
  starts a task and not a radio;
* **associated is not online.** Once a minute, while there is a link, the manager
  pings one of `internet.targets` and sees. A router with no uplink, a captive
  portal and a guest network that only allows port 80 all look like a healthy
  connection from the station's side. §10.17 gives the two states the same colour
  and the same rhythm — they are both yellow — but they are separate rungs in the
  ranking and `wifi` on the console tells them apart.

The list is three anycast resolvers rather than one address, because plenty of
otherwise usable networks drop ICMP to one operator or another and one blocked
host must not read as an outage.

## 10.14 The language and the layering

### 10.14.1 No heap

Unchanged, and worth reading in the sibling folder. Everything this firmware owns
is static: task stacks are `StackType_t` arrays with `xTaskCreateStatic`, mutexes
are `xSemaphoreCreateMutexStatic`, buffers are file-scope arrays.

**Three exceptions, all of them at a boundary and all of them one-shot:**

| Where | What | Why it is allowed |
|-------|------|-------------------|
| `config.cpp`, `fido.cpp` | cJSON allocates while building or parsing a tree | a library's heap use in a one-shot path; the *output* goes into our own buffer, so a config that outgrows its cap fails there rather than in the filesystem |
| `fido_usb.cpp` | two 64-byte `usb_host_transfer_alloc` buffers | there is no static form of it. Allocated when a key is plugged in, freed when it is unplugged. **Not per exchange, and it must never become per exchange** |
| the framework | Wi-Fi TX buffers, lwIP pools, the USB Host Library's descriptors | not ours to decide, and this board has 8 MB of PSRAM behind them (§10.13) |

`std::string` is the sibling folder's named exception and does not appear in this
firmware at all.

**Fixed capacity makes "full" a state that has to be designed**, and every one of
them is: four pending requests (`ui::RequestCard::kMaxPending`), two decisions
waiting to be signed (`responder::kPendingDecisions`), four Wi-Fi networks, four
ping targets, one FIDO interface. Over capacity is a *drop*, which is §10.10's
fail-safe, and it is counted.

### 10.14.2 Library layer first, logic second

Unchanged in principle, and this board makes it easier to see because there is so
little hardware:

```
components/led          a serial LED           knows nothing about approvals
components/buttons      a contact that bounces knows nothing about which board
components/boards       the pin map            puts the two together
components/indicator    a ranking              knows nothing about a radio
components/fido         a key on a cable       knows nothing about a verdict
components/responder    the loop               knows about all of it
main/                   composition            the only file that may know everything
```

Two seams worth naming because both are enforced by a header rather than by
convention:

* **`buttons` takes pins as arguments and does not include `board.h`.** This layer
  knows about contacts, not about which board they are soldered to;
* **`indicator` takes a struct of booleans through a gatherer `main` registers.**
  It has never heard of Wi-Fi. §10.17.4 has the argument, and the test is that
  deleting `responder` leaves a working indicator.

### 10.14.3 The bus that is not here

The sibling folder's §10.14.3 is about leasing an I²C bus between five chips and
the three driver bugs that found. **There is no I²C bus on this board** (§10.13),
so that section has no counterpart here — which is most of why this firmware is a
third of the size.

What replaced it as the one shared, contended resource is the **USB port**:
`fido::usb::Exchange` takes a mutex for the length of one CTAPHID conversation,
because the console and the responder can both want the key and a second command
interleaved into a conversation is a wedged channel.

### 10.14.4 The house firmware

`E:\projects\Zesec.ModuleX.Firmware.v3` is the codebase this project borrows
shapes from, and on this board it contributed something concrete rather than a
shape: **the whole WS2812-over-UART trick** in §10.17.3, numbers and all — the
baud rate, the six-bit frame, the inversion, the four-character table and the
sixty-step Weber–Fechner breath. Those were chosen against a real emitter on a real
desk, and re-deriving them from nothing would have been re-deriving them worse.

## 10.15 The settings file

`config.json` on the SPIFFS partition, parsed into a fixed struct. Three rules,
all unchanged:

* a `config.json` that is missing, oversized or unparseable is **restored from
  `config.init.json`**, with one log line, and boot continues;
* a write goes to `config.json.new` and is then renamed over the original,
  because a power cut in the middle of the recovery path is the one failure that
  would break recovery itself. SPIFFS will not rename onto an existing name — it
  answers EIO — so there is a real window, and `RecoverInterruptedWrite` closes it
  at the next boot;
* **unknown fields are ignored and lost on the next write.** That is the honest
  behaviour of a fixed struct: a file written by a newer firmware does not survive
  a downgrade.

### What is in it that is not next door

```json
"led":      { "brightness": 15, "idleBrightness": 7 },
"approval": { "requireKey": true, "touchTimeoutSeconds": 30, "denyButton": true }
```

and what is *gone*: `display`, `touch`, `audio` (no such hardware) and `web`
(no web server here). §10.17.2 and §10.18.4 argue the two that replaced them.

### The restore, and the one place this board differs

Holding the button restores `config.init.json` over `config.json`, leaving
`registration.json` and `fido.json` alone — three lifetimes, three files.

**But the button is read *after* boot, not through the reset.** `BOOT` is GPIO0
and GPIO0 held across a reset is the ROM's download strap (§10.1), so a restore
asked for that way would be a restore nobody could perform. The rule here is:

> Press and hold BOOT **while the board is coming up**, and keep holding.

If the pin is high when `app_main` looks, nothing happens and the boot costs one
GPIO read — which is every ordinary boot. If it is low, the LED goes **white,
solid** and the five-second hold begins.

That last part is the thing this board has that the sibling's does not: its five
seconds are *blind*, because there is no screen and no sound that early. Here the
LED is already up before the window opens, so the hold has feedback. It is the one
place where having a single emitter instead of a panel is an improvement.

The restore still runs **before** `config::Init()` parses the file, because the
failure it exists for is a `config.json` that stops the device booting, and a
restore that ran after the parse could not rescue it.

### Who is told when the fields move

A reload or a restore replaces every field at once, and three subsystems hold
copies: the LED a brightness, the Wi-Fi manager a network list, the clock's sync
task an interval and a server, the bus a URL. Telling them is not `config`'s job —
it has never heard of a UART — but *remembering* to tell them cannot be the
caller's either, because there are two callers.

So it is a hook: `main` registers `SettingsChanged`, and `Reload`/`Restore` call
it themselves on success. `Init` deliberately does not — it runs before any of
those subsystems exists, and `main` applies each of them explicitly in an order
written down there.
