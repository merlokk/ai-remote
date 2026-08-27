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

### 10.14.3 The one contended resource

**There is no I²C bus on this board and no chip that would want one** (§10.13), so
there is no bus lease here and no epoch bookkeeping in the host tier's fake — which
is most of why both are small.

The one shared, contended resource is the **USB port**:
`fido::usb::Exchange` takes a mutex for the length of one CTAPHID conversation,
because the console and the responder can both want the key and a second command
interleaved into a conversation is a wedged channel.

### 10.14.4 The house firmware

**The house firmware** is a separate, unrelated ESP-IDF codebase on this machine
that this project borrows shapes from — settled answers to problems this one meets
again, rather than any code that is linked or vendored here. On this board it
contributed something concrete rather than a shape:
**the whole WS2812-over-UART trick** in §10.17.3, numbers and all — the
baud rate, the six-bit frame, the inversion, the four-character table and the four
blink rates. Those were chosen against a real emitter on a real desk, and
re-deriving them from nothing would have been re-deriving them worse.

**One number came across and turned out to be wrong here, and §10.17.5 is the
post-mortem**: the breath's sixty-step ramp, which was the perceptual curve where
it needed the curve's *inverse*. Its nine-second period was right and was kept.

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
"approval": { "touchTimeoutSeconds": 30, "denyButton": true }
```

and what has **no section at all**: there are no `display`, `touch` or `audio`
fields (no such hardware), and no `time` one — there is no clock on this board and
nowhere to show one (§10.13). §10.17.2 and §10.18.6 argue the two above.

**`web` is not on that list any more**, and it is the sibling board's section
unchanged (§10.16):

```json
"web": { "mode": "auto", "write": true, "user": "", "password": "" }
```

`auto` is the cheap default — the server comes up only while this device is its own
access point, which is the one state in which somebody has no other way to reach it.
The credential is **empty by default, and the pair is the switch**: both set is a
locked site, either half missing is an open one, and there is no third boolean
beside them for the reason `Wifi::active` has none. A device flashed with the
shipped file therefore serves to whoever can reach it, exactly as this board did
before the server existed — and `web` on the console says `OPEN` in those words
when only one half is filled in.

**Four of these six sections cannot be written from the site**, which is where
§10.15 meets §10.10 rule 4 on this board: the write path whitelists `wifi` and
`nats` and refuses everything else by name — `approval` (when a verdict may be
asked for), `led` (what this device is saying while it asks), `web` itself, and
`internet` (the reachability probe, which is nobody's business over HTTP).
[`web.md`](web.md) argues the first three; two host tests are the enforcement.

**`requireKey` was here and is gone**, which is worth a line because a config file
left over from before will still have it. It switched the security key off and let
the button approve alone; since §10.18 the private key lives inside the
authenticator, so there is no such mode and nothing for the field to switch. A file
that still carries it loads fine, the field is ignored, and the next write drops
it — §10.18.6 has the argument and a host test pins the behaviour.

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

**The white is the part worth having**: the LED is up before the window opens, so
a five-second hold has feedback rather than being blind. It is the one place where
having a single emitter and nothing else is an improvement — a display would not
be initialised this early, and there is no sound on this board at all.

The restore still runs **before** `config::Init()` parses the file, because the
failure it exists for is a `config.json` that stops the device booting, and a
restore that ran after the parse could not rescue it.

### Who is told when the fields move

A reload or a restore replaces every field at once, and four subsystems hold
copies: the LED a brightness, the Wi-Fi manager a network list, the bus a URL, and
the configuration site its mode — so a `config reload` saying `web.mode: off` can
take down the page that asked for the reload (§10.16).

**The gate is not one of them**, which is worth stating because it looks like it
should be: `approval.touchTimeoutSeconds` is read when the next request arrives, so
there is nothing to re-apply and a timeout changed mid-request does not move the
request already waiting.

Telling them is not `config`'s job — it has never heard of a UART — but
*remembering* to tell them cannot be the caller's either, because there are two
callers.

So it is a hook: `main` registers `SettingsChanged`, and `Reload`/`Restore` call
it themselves on success. `Init` deliberately does not — it runs before any of
those subsystems exists, and `main` applies each of them explicitly in an order
written down there.
