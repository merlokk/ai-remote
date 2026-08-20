# approver-esp32 — the firmware's own housekeeping, and the rules it is written to

This file owns **§10.9** Wi-Fi — the radio and the manager above it — **§10.14**
the language, the layering and the no-heap rule every component in this folder
obeys, and **§10.15** where the configuration lives and the button that puts it
back. Section numbers are global and stable ([`../CLAUDE.md`](../CLAUDE.md) §2),
so each keeps its number here — and they are in **number order rather than
narrative order**, so that a `§10.9` in a code comment is found by scrolling
rather than by reading.

What is here is what the device does **for itself**: get onto a network, keep
itself there, know what it was told to do, and be recoverable when what it was
told is wrong. Nothing below can approve anything. **If you are reading front to
back rather than looking something up, start at §10.14** — it is the section every
other one in these documents assumes, and the reason there is no `new` anywhere.

Two subsections are deliberately elsewhere. **§10.14.3**, the leased I²C bus, is
in [`hardware.md`](hardware.md), because it is the contract the drivers obey and
where three driver bugs were found. And the path a verdict travels — the bus
client, the key and the registration — is [`protocol.md`](protocol.md).

Where the rest of §10 lives:

- [`CLAUDE.md`](CLAUDE.md) — what this device is, its status, and the map;
- [`protocol.md`](protocol.md) — **§10.5** the NATS client, **§10.6** the key,
  **§10.7** registration and the console;
- [`hardware.md`](hardware.md) — the board, its drivers, and the I²C lease;
- [`screens.md`](screens.md) — the seven screens, including the Wi-Fi one this
  manager sits under;
- [`web.md`](web.md) — the configuration site, and the settings it may write;
- [`tests.md`](tests.md) — every rule below, as an assertion, with no board;
- [`build.md`](build.md) — the dependency set and what each part costs;
- [`commands.md`](commands.md) — `wifi`, `config` and the rest, as a reference;
- [`working-with-code.md`](working-with-code.md) — how to build, flash and talk
  to it.

### 10.9 Wi-Fi — the manager behind that screen

Everything else in this repository runs on a machine somebody already logged
into. This device has to get onto a network by itself, keep itself there, and
behave sanely when it cannot — and none of that may involve a laptop.

**Two components, and the split is the design.** `components/wifi` is the
radio: join this network, be an access point, what is the link doing, what is
on the air. `components/wifimgr` is everything that decides *which* and *when*.
The driver has no opinions and the policy has nothing but opinions — and the
reason to draw the line there is §10.11: `wifi_policy.h` includes `<cstdint>`
and nothing else, exactly as `ui/navigator.h` does, so the whole of the
behaviour below runs under Unity on the host with no board and no fake. The
half that cannot be tested that way is the half with no decisions in it.

**Desired and current are two different facts, and both are on show.** The
operator asks for **off**, **client** or **AP**; what is happening on the way
there is `client "point1" connecting`, `temporary AP "approver-esp32"`,
`client "point3" connected`. A readout that showed only the first would make a
device that is trying look like a device that is broken, and one that showed
only the second would lose the question the device is answering.

**A state machine, owned by one task, driven by polling the driver:**

```
NO_CREDENTIALS ──join──► CONNECTING ──got IP──► ONLINE ──disconnect──► RETRYING
      ▲                      │                                             │
      └──── forget ──────────┴────── auth fail / not found ────────────────┘
```

and, wrapped around it, the loop a device with no keyboard actually needs:

```
        ┌──────────────── each remembered network in turn ───────────────┐
        │   net 1 ──fail──► net 2 ──fail──► … ──fail──► (round over)     │
        └───────────────────────┬───────────────────────────────────────┘
              round < rounds_before_ap │ backoff, growing and capped
                                       ▼
                         ┌── round == rounds_before_ap ──┐
                         ▼                               │
                  temporary access point  ──nobody came in 2 min──┘
                         │
                    somebody attached → the clock stops
```

- **Round-robin, then be findable.** `rounds` full passes over the list (§10.9's
  "2-3", `config.json`), and then the device stops being a client and *becomes*
  an access point so that somebody can reach it and tell it about a network that
  exists. If nobody turns up inside `apWindowSeconds`, it goes back to trying.
  Forever, and never in a tight loop.
- **A station attached holds the window open.** They are the entire reason it
  exists; a device that dropped the operator's phone mid-form to go and retry a
  network that was not there is a device nobody can configure. When the last one
  leaves the two minutes start again from the beginning rather than resuming —
  somebody who joined at 1:59 must not leave the next person four seconds.
- **The window expiring clears the sticky auth failures**, because that window
  was the chance somebody had to fix a password. Without that, a device with one
  wrong password would be unfixable by the very mechanism built to fix it.
- **An exhausted list does not wait out its rounds.** Every network refusing the
  password, or no networks at all, sends the access point up at once: a device
  sulking in silence for a minute when it could be findable is a device nobody
  can rescue.
- **An attempt that is never answered times out** (15 s). An AP that associates
  and then never finishes — a captive portal, a marginal link — would otherwise
  mean the device never reaches its second network.
- **A drop while online is not a refusal**, and goes back through the gap rather
  than straight into a reconnect. A link that worked and then dropped with a
  handshake error is a link that lost its AP, not a password that is wrong;
  marking it sticky would strike a working network off the list, and
  reconnecting instantly would turn an AP that is kicking us into a loop running
  as fast as the radio can associate.
- **`mode: "ap"` and the fallback AP are the same access point for different
  reasons**, and they are two states rather than one: the first is an answer,
  the second is a symptom, and a screen that spelled them the same way would be
  lying about whether anything is wrong.

- `NO_CREDENTIALS` is a first-class state, not an error: fresh from the flasher,
  the clock still runs and the Wi-Fi screen is what the device asks for.
- `RETRYING` backs off — a few seconds, then tens, capped at a minute or so —
  and **never becomes a tight loop**. A device retrying a dead AP ten times a
  second is a device that heats up, drains a battery and floods the log.
- An **auth failure is sticky and is reported**, not retried forever. Wrong
  password and "the AP is out of range" are different problems and the screen
  must not spell them the same way (`WIFI_EVENT_STA_DISCONNECTED` carries a
  reason code — use it).
- Only `ONLINE` releases the bus task. The NATS client waits on "got IP", and on
  the way down it tears the socket rather than letting it hang on a dead route.

**Storage.** Credentials live in `config.json` (§10.15), not in the driver's own
store: set `esp_wifi_set_storage(WIFI_STORAGE_RAM)` so there is exactly one
record of what this device knows, in one place, and one restore that clears it.
§10.15 states what that costs — SPIFFS cannot be encrypted, so the WPA key is
readable from a flash dump. Remember a small number of networks (four is
plenty), try last-successful first — a desk device that moves between a home and
an office is the entire use case; roaming between dozens is not.

**What the radio can and cannot do**, because these are support questions, not
bugs:

- ESP32-C6 is **2.4 GHz only**, and this is hardware rather than a setting: the
  part has one radio. ESP-IDF marks a dual-band chip with `SOC_WIFI_SUPPORT_5G`
  — defined for the ESP32-C5, absent for the C6 (`soc/esp32c6/include/soc/
  soc_caps.h`, read rather than assumed), so there is no band mode to select
  and no 5 GHz channel bitmap to fill in. A 5 GHz-only SSID is not missing from
  the scan; it is inaudible.

  Two places say so rather than one, because "why can't it see my network" is
  the support question this section exists to pre-empt: every `wifi scan`
  prints it, and `wifi_radio.cpp` carries a `#error` under
  `SOC_WIFI_SUPPORT_5G` so that a port to a part which *does* have the band
  fails the build instead of quietly scanning half of it.
- WPA2 and WPA3-SAE personal, open networks, hidden SSIDs by manual entry.
  **WPA2-Enterprise and captive-portal networks are out of scope** — a device
  that cannot show a login page cannot join a network that demands one, and a
  hotel Wi-Fi is not where this thing approves `rm -rf`.
- A weak signal is not a failure state. Show RSSI and let the operator decide;
  reconnect logic must not treat a marginal link as a reason to forget anything.

**Why the screen and not `wifi_provisioning`.** ESP-IDF ships SoftAP/BLE
provisioning with a phone app on the other end, and it is the right answer for a
headless sensor. This board has a 480×480 touchscreen: pulling a phone, an app
and a second radio into onboarding would be more moving parts to reach a worse
result. The cost is honest — typing a 30-character WPA key on a touchscreen is
unpleasant, exactly once per network — and it is the reason the keyboard gets a
show-password toggle instead of a policy against showing it. **§10.8.6 has that
cost in millimetres now**, which is what turned "unpleasant" into a layout: this
glass is 38.8 mm across, so `lv_keyboard`'s ten columns are 3.5 mm apart and the
keyboard is a 6×5 grid of 6 mm keys instead.

**The registration token still comes over USB** (§10.7). It is ~50 characters of
base64 that must be transcribed exactly, it is minted on the host anyway, and a
typo in it fails in a way that looks like a protocol problem. Wi-Fi is typed once
and confirmed by the device joining; a token is pasted once and confirmed by the
handler's signature.

#### What is written, and the four decisions inside it

`components/wifi` (the radio) and `components/wifimgr` (the policy and the task
that drives it), plus `wifi` on the console (§10.7) and the settings below in
`config.json`.

**What the board has actually done**, flashed and driven from the console
against the shipped `YOUR_SSID` — a network that by construction does not
exist: brought the radio up on demand, tried the network, been told `reason
201` and reported it as *no such network*, waited out a backoff it printed as
`next attempt in 3805 ms`, spent its two rounds, put `approver-esp32` on the
air with a DHCP server on 192.168.4.1 and a 120-second window counting down,
scanned eight neighbours while doing it, switched to a permanent AP and back to
off — and, from a cold `state off / radio not started`, answered `wifi scan`
with eight networks and went back to off without anything else being typed. A
fixed address is set, refused when it is not one (`010.0.0.42`, `10.0.0.300`),
saved, reloaded and visible in `cat config.json`.

**And then it joined one.** Given a real SSID and password it walked the list —
`YOUR_SSID` refused as `reason 201`, the second network associated — and came
up on DHCP; a fixed address set on that network took at `STA_CONNECTED` in
**69 ms** against DHCP's two seconds, and turning it off put the DHCP client
back with a real lease rather than a leftover address. The internet check
answered `online` on the first round, went `unknown` then `offline` over two
rounds against an address in TEST-NET-1 that answers nothing, and came back to
`online` on the first reply after the real targets were restored.

What is still device-tier and unreached: the auth-failure classification, which
needs a network whose password is deliberately wrong.

Two things only the board could have said, both now fixed:

- **`wifi scan` printed "joining 'YOUR_SSID'" five times for one join.** The
  manager retries an action the radio refused while a scan holds it — correct,
  and the log line was on the wrong side of that refusal. It is emitted after
  the call is taken now, so a retry is silent.
- **`state temporary ap 'YOUR_SSID' … round 3 of 2`.** Both halves were true
  internally — the network index is the last one tried and the round is
  incremented before it is compared — and both are nonsense on a screen. The
  network clause is now printed only in the states that are about a network,
  and the round is clamped to its own total.

Four things in the driver are decisions rather than plumbing:

- **The access point is `APSTA`, not `AP`.** The station interface stays up and
  unconnected so that a scan works while the fallback AP is the only thing
  running — which is precisely the moment somebody needs to pick a network. In
  plain `WIFI_MODE_AP` that scan is not possible at all.
- **A disconnection this firmware asked for is not a failure.** Tearing an
  association down to start another one produces a `STA_DISCONNECTED` of our
  own, and counting it would fail the *new* attempt before it began. A
  suppression counter, incremented before every deliberate disconnect, is the
  subtlest thing in the file.
- **`kFailed` is latched** until the next `Start…`, so a poller that looks a
  moment too late still sees the edge. That is what lets the manager be a poll
  loop rather than a queue.
- **The station's authmode threshold stays at `WIFI_AUTH_OPEN`**, which is not a
  security decision: it is the *minimum* an AP may offer, and raising it to WPA2
  makes an open café network and a WPA3-only router both vanish. What protects
  the link is the AP's own security, never our floor.

And one in the manager: **the radio is brought up lazily.** The shipped
`config.json` has `wifi.active` false, and a device configured with the radio
off should pay nothing for this component existing. The static cost that is
always paid is 1.5 KB of scan buffer and a 4 KB task stack — `idf.py
size-components` puts `libwifi.a` at 6,260 bytes and `libwifimgr.a` at 9,457
(5,098 and 7,379 when this was first written — §10.12 says why these are a
snapshot), against the framework's ~415 KB of flash for `net80211` + `pp` +
`lwip` that
arrive the first time any of it is linked.

**What laziness is saving is 41 KB of RAM, and off gives it back.** Bring-up is
in two halves for exactly that reason:

| | Cost | Given back? |
|---|---|---|
| once ever — NVS, `esp_netif`, the default event loop, our handlers on it | ~10 KB | no |
| per bring-up — the default netifs and `esp_wifi_init` | ~41 KB | **yes, on `Stop`** |

Measured with `status` at a steady state well after boot: **170,524** bytes free
having never touched the radio, ~**160,000** once it has been used and switched
off, **119,256** with a client running, 116,336 low-water. Twenty-six on/off
cycles on the board return it every time and drift a few hundred bytes in
*both* directions — the allocator, not a leak.

The split exists because the two halves have different lifetimes and one of
them **cannot** be repeated: `esp_netif_create_default_wifi_sta()` may not be
called twice for the same interface, so the netifs are created in `EnsureStack`
and destroyed in `ReleaseStack` rather than living forever, while the event
handlers — which are attached to the loop, not to the driver — are registered
once and survive the stack going up and down underneath them.

**A correction worth recording, because the first number written here was
wrong.** This section originally claimed 130 KB, from a `status` taken two
seconds after boot against one taken after the radio came up. That first
reading is *before LVGL takes its 64 KB pool* — the splash and the boot chime
mean LVGL starts about five seconds in — so it was comparing two different
moments and charging the difference to Wi-Fi. Same-instant measurements are
above. A heap number without the point in the boot it was taken at is not a
measurement, which is the lesson rather than the arithmetic.

**`wifi scan` works with the radio off**, which is the shape the operator
needs: "what is out there" is the question you have *before* the device is
anywhere. The driver brings the station interface up, scans, and puts it back
exactly as it found it. **No access point is raised to do it, hidden or
otherwise** — hidden means the SSID field in the beacon is blank, not that the
AP is silent, so it would still beacon, still hold a channel and still start a
DHCP server, all to make possible something a station does on its own. What is
on the air during a scan is probe requests and nothing else.

**The settings, in `config.json` (§10.15):**

| Field | What it is |
|---|---|
| `wifi.active` | the one switch that means radio-down. False is off whatever `mode` says — two fields that can disagree is one bug report nobody can read |
| `wifi.mode` | `"client"` or `"ap"`. **Anything else is refused and the default kept**, not guessed: the same call §10.8.2 makes about a misspelled zone |
| `wifi.rounds` | full passes before the fallback AP. `0` behaves as 1 rather than as "never try" — the round is counted before it is compared, so every network gets one attempt whatever this says |
| `wifi.apWindowSeconds` | how long that AP stays up with nobody on it |
| `wifi.ap.{ssid,password,channel}` | the access point this device raises. **Whether it is protected is the config's answer, not the firmware's** — see below |
| `wifi.networks[]` | up to four ssid/password pairs — each with an optional `ip` block, below |

The timings that are **not** in the file — the connect timeout, the gap between
attempts, the backoff and its cap — are the shape of this section rather than a
preference. A device whose connect timeout is operator-settable is a device with
one more way to be configured into never working.

**The device's own access point can be protected, and `wifi.ap.password`
decides — it is a setting, not a property of the firmware.** Three cases, and
the third is the one worth having a rule for:

| `wifi.ap.password` | What goes on the air |
|---|---|
| empty | an **open** network |
| eight characters or more | **WPA2**, and the console says `wpa2` rather than `open` where it prints the fallback AP |
| one to seven | **refused** by the driver, and the AP does not come up |

That last row is deliberate and is a driver rule rather than a config one:
WPA2 will not take a passphrase shorter than eight, and the tempting failure
mode — accept it and raise an open network instead — is an access point
somebody believes is protected. Refusing is the honest answer, and `wifi`
showing `open` next to a password that was set is the symptom to look for.

**The two shipped files disagree about this on purpose, and the consequence is
worth knowing before it surprises somebody**: `config.json` raises a WPA2 AP
and `config.init.json` — the factory defaults — leaves it open, so a **restore
(§10.15, or holding `KEY` at boot) opens the access point**. Neither is more
correct than the other. A key committed to this repository is a key everyone
with the repository has, so it is a lock against a casual neighbour rather than
a secret; and while that AP still serves nothing and stays up two minutes at a
time, an open one is not much of an exposure either. When §10.8.6 gives it a
screen to serve — and a WPA key typed into it — this is the line to revisit,
and the two files should stop disagreeing at the same time.

#### Is there an internet through it?

**Associated is not online, and the device needs both facts.** A router with no
uplink, a captive portal, a guest network that only allows port 80 — from the
station's side every one of them looks like a healthy connection. So: while
there is a client link, ping one of a few addresses once a minute and see.

```json
"internet": { "check": true, "intervalSeconds": 60, "timeoutMs": 2000,
              "failures": 2, "targets": ["8.8.8.8", "1.1.1.1", "9.9.9.9"] }
```

**A list rather than an address**, because plenty of usable networks drop ICMP
to one operator or another and one blocked host must not read as an outage.
Three anycast resolvers is the shipped default; `wifi check <address>…`
replaces them and `wifi check off` stops asking. A target that is not an IPv4
address is **refused, including a hostname** — there is no resolver in an ICMP
echo, so `google.com` there would be a check that can never pass, which reads
as an outage that never ends.

The rules, all of them in `reachability.h` and all host-tested:

- **Three states, and the third is the honest one.** `unknown` is what "no
  link", "checking is off" and "the first round has not answered yet" all mean.
  A device reporting offline because it had not looked would be lying with a
  straight face, and `LinkDown` therefore goes back to `unknown` rather than to
  `offline` — no link is a fact the Wi-Fi state already shows, and putting two
  red marks on a screen for one problem is how a screen stops being read.
- **A round is the whole list.** A target that does not answer is followed by
  the next one *immediately*, not next minute: three addresses would otherwise
  take three minutes to conclude anything. Only when every one of them ignores
  us is the round a failure.
- **Going offline is slow, coming back is instant.** Two consecutive failed
  rounds before the word "offline" — one lost round is a lost packet, a roaming
  beacon, a busy router — and exactly one reply to be online again. The
  asymmetry is the design: an outage that has ended is over, and making the
  operator wait two more minutes to be told so is making them reboot the device
  instead.
- **The address that answered goes first next time**, the same idea the network
  policy applies to last-successful, and for the same reason: without it every
  round opens with a host this particular network drops.
- **It does not feed back into which network to join.** A link that carries no
  traffic is a fact to report, never a reason to drop it and try the next one —
  that would turn one dead uplink into a device cycling through its networks
  forever, and the operator would see a device that cannot connect rather than
  a router that needs restarting.

The ICMP itself is `esp_ping` in the manager and is four lines: one echo per
probe, a session created and deleted around each one (the library cannot
retarget a live session, and once a minute is nowhere near often enough for the
task churn to matter), plus **our own deadline on top of its timeout** — §10.5's
rule about bounding every read, applied to somebody else's task, because a
session that never calls back would otherwise freeze the check for good.

**One bug the board reported immediately, and the shape of it is worth
keeping**: `wifi check 8.8.8.8` tore down a working connection and walked the
network list from the top again. The setter called `wifimgr::Apply()`, which
reconfigures the *network policy* — and reconfiguring a policy restarts it, by
design, because the network list may have changed under it. The ping list is
not the network list. `ApplyInternetCheck()` exists now and touches only the
probes; `Apply()` is for the settings that really do invalidate a connection.
The `E ping_sock: send error=0` in the same log was the consequence rather than
a second fault — a probe leaving as the interface went down.

The general rule that came out of it: **a settings call that reconnects is a
settings call people stop making**, so each one should reach for the narrowest
thing that has actually changed.

**This is the seam SNTP hangs off, and it is taken now** (§10.8.2).
`components/timesync` reads `Snapshot::internet` rather than growing a second
probe of its own — with one wrinkle worth recording here, where the states are
defined: it treats `kUnknown` as **permission to try**, not as a refusal. Only
`kOffline` stops it. The check being switched off is not a statement that there
is no internet, and a device whose operator turned the ping off must not
quietly lose its clock as well.

#### A fixed address, per network

DHCP unless a network says otherwise, and the "otherwise" hangs off the network
rather than off the device:

```json
{ "ssid": "office", "password": "…",
  "ip": { "static": true, "address": "10.0.0.42", "netmask": "255.255.255.0",
          "gateway": "10.0.0.1", "dns1": "10.0.0.1", "dns2": "" } }
```

**Per network is the half of the house firmware's shape worth copying**
(§10.14.4 — its `WifiCredentials::StaticIP` sits in exactly the same place). A
desk object that moves between a home that hands out addresses and an office
that hands out nothing needs one of each, and a single device-wide setting
would make the two mutually exclusive. `wifi static <n> <address> <netmask>
<gateway> [dns1] [dns2]` sets one from the console and `wifi static <n> off`
puts that network back on DHCP, both memory-only until `config save`, like
every other setter (§10.15).

Where it does **not** follow the house:

- **The address is parsed, not just checked for being non-empty.** Theirs
  validates by testing the five strings against `""`; empty is not how an
  address is usually wrong — `192.168.1.` and `10.0.0.300` are. `ParseIpv4` is
  strict about all of it and **refuses a leading zero** rather than picking one
  of its two meanings: `010` is ten to the person who typed it and eight to
  `inet_aton`, and a device quietly on 8.1.1.1 is an evening nobody gets back.
- **The DNS entries are optional.** Theirs requires both before it will call a
  static config valid. A LAN with no resolver is ordinary, and §10.3's bus is
  reached by address rather than by name.
- **Three fields or none, and the SSID survives either way.** A static block
  that is enabled but missing an address, a netmask or a gateway falls back to
  DHCP with one log line naming the field. Refusing the whole entry would lose
  a working network over a typo in an optional field; honouring half of it
  would give an interface with an address and no route, which looks connected
  and reaches nothing.
- **The driver is handed a copy, not a pointer.** Theirs keeps an `IPConfig*`
  into the config object and dereferences it from an event handler; here the
  console can edit the network list between an association starting and the
  event arriving, so `StartClient` takes the binary form by value.
- **The DHCP client is put back.** The netif outlives one association, so a
  network with a fixed address leaves the client stopped for whatever is joined
  next. Theirs destroys and recreates the netif per connect and never meets
  this; here `ApplyAddressing` starts the client again when the network being
  joined has no address of its own. Without it, static-then-DHCP is an
  interface that never asks for an address and a device stuck at "connecting"
  with nothing to show for it.

Two things that are the same because they are simply right: the address goes on
at **`WIFI_EVENT_STA_CONNECTED`** — associated, and before the DHCP client has
got anywhere — and the order is stop asking, then say what the answer is.
`esp_netif_set_ip_info` on an interface that is already up raises
`IP_EVENT_STA_GOT_IP` itself, so the link reaches `kConnected` through the one
path that also serves DHCP; that looks like an omission in the code and is not.

Text in the file, binary at the driver, and `config::ParseIpv4` between them —
in the config layer for the reason `tz::Lookup` is there: turning what the file
says into what the hardware takes is the file's job, and it keeps both halves
testable without a board. What `cat config.json` and the console show is the
string that was typed, never a number somebody re-rendered.

### 10.14 How it is written — the language, and the layer that comes first

#### 10.14.1 C++, C only where forced, and no heap

**The firmware is C++.** C is not the default and not the fallback of habit; it
is what a specific spot forces. The places that force it are few and knowable:
anything that must present a C ABI to ESP-IDF or to a C component, `extern "C"`
entry points (`app_main`, event and ISR handlers registered with IDF), and
headers meant to be included from C. Everything else — the drivers of §10.14.2,
the bus task, the screens' glue, the protocol assembly of §10.2 — is C++.

This is also what makes §10.4's dependencies fit rather than fight:
`debsahu/espidf-nats` is header-only **C++** and now needs no wrapper for
language reasons; LVGL, libsodium and IDF itself are C with `extern "C"` in
their own headers, which is all that is needed to call them.

The dialect is the embedded one, and the constraints are not stylistic:

- **No exceptions, no RTTI** — off by default in ESP-IDF and staying off.
  Errors are return values (`esp_err_t` at the IDF boundary, a small result type
  above it), which is also what §10.10's "no reply is the safe outcome" needs:
  every failure path has to be a value someone decided about, not a stack unwind.
- **No dynamic memory** — the rule below, and the one that shapes the most code.
- **RAII is the point, not decoration.** A lease that releases itself on every
  return path (§10.14.3), a lock that cannot be forgotten, a socket that closes
  when its owner dies. This is most of why the language is worth having here —
  and note that RAII here manages *ownership of a resource*, never a lifetime of
  memory.
- Compile-time over run-time where it is free: `constexpr` sizes, `enum class`
  for states (the machines of §10.8.1 and §10.9), `static_assert` on the wire
  constants of §10.2.

**No heap: everything is allocated statically and lives forever.** 512 KB of
SRAM shared with lwIP, Wi-Fi and TLS, and no way to add PSRAM (§10.1) — so our
code has no `new`, no `delete`, no `malloc`, and — with the one named exception
below — nothing that grows. Objects are constructed once, at file or class
scope, and stay for the life of the device. Almost nothing has an allocation
failure to handle because almost nothing allocates, and a device meant to sit on
a desk for months does not accumulate fragmentation.

What that means in practice, including the parts that are not obvious:

- **No `std::vector`, no `std::function`.** `char[N]` with an explicit length,
  fixed arrays, spans over them, and plain function pointers with a
  `void* user_data` — which is the shape LVGL and IDF callbacks want anyway. And
  **no fixed-container library**: pulling in `etl` or similar would be a new
  dependency under root §1, and a handful of small containers written here is
  cheaper than that conversation.
- **`std::string` is allowed where it earns it** — the one named exception, and
  it is a real one: `debsahu/espidf-nats` is a C++ API, and text that arrives as
  a `std::string` is not worth copying into a `char[N]` to satisfy a rule. Use
  it, knowing what it does: libstdc++ keeps up to **15 characters inline** and
  goes to the heap past that, and every append can reallocate. So —
  - fine in setup and one-shot paths: parsing a config, the registration
    exchange (§10.7), console commands, building a URL;
  - **not** in an ISR, not in the LVGL frame path, and not as the long-lived
    home of something a fixed buffer already holds — a `tool_input` bounded by
    §10.10 belongs in the buffer that bounds it;
  - pass `const std::string&` or `std::string_view`, never by value, and
    `reserve()` once if it will grow;
  - "objects live forever" is unchanged by this: a static object holding a
    `std::string` still takes its buffer from the heap the first time it grows.
    The low-water heap mark below is what tells you whether that mattered.
- **FreeRTOS objects are static too** — `xTaskCreateStatic`, `StaticQueue_t`,
  `StaticSemaphore_t`, with the stacks and storage as arrays. The dynamic
  variants take from the heap, which is the thing we are trying not to depend
  on.
- **Constructors run before `app_main` and before any driver exists.** A static
  object whose constructor touches I²C, NVS or the network is a boot crash with
  a stack trace that names the wrong thing. Two phases: a trivial constructor,
  then an `init()` that may fail and returns a value. And because the relative
  order of static constructors across translation units is undefined, the
  `init()` calls are made from `app_main` in an order that is written down.
- **One composition root owns everything, and it is what makes "static" work.**
  A single object, reached through a function-local static
  (`static Root instance; return instance;`), holding every subsystem **by
  value** as a member and handing out references. That gives all of it at once:
  no allocation, lifetimes equal to the device's, construction in member
  declaration order — deterministic, and *inside* `app_main` rather than before
  it, because the first `getInstance()` is the thing that builds it. The
  static-init-order problem is not mitigated; it stops existing.
- **Fixed capacity makes "full" a state that must be designed**, not an error to
  report later. The pending-request queue of §10.8.4 ("+2 waiting") is N slots;
  request payloads have a maximum size, which §10.10 requires regardless because
  the input is untrusted. Over capacity → drop with one log line and no reply,
  which is already the fail-safe (§10.10), never a reallocation.
- **The libraries still allocate, and that is the part to watch.** lwIP, the
  Wi-Fi stack, mbedTLS, libsodium and `debsahu/espidf-nats` all use the heap;
  this rule binds our code, not theirs. What our code owes them is headroom:
  don't hold large buffers they will need, and treat
  `heap_caps_get_minimum_free_size()` — the low-water mark, not the current
  free heap — as the number that says whether the device is safe. §10.8.5's
  About screen shows free heap; it should show that one.
- **LVGL is the one exception, and it is bounded on purpose.** LVGL objects come
  from its own pool, sized once in `lv_conf.h`. So: build every screen at boot
  and keep it, never create and delete widgets per navigation — which is what
  §10.8.1 already requires when it says what was underneath comes back with its
  scroll position and its half-typed password intact. A screen that is rebuilt
  is a screen that lost its state and touched the allocator to do it.
- **The numbers are recorded, not assumed.** `idf.py size-components` (§10.12)
  for static footprint, and the low-water heap mark under load — a request
  arriving during a Wi-Fi scan with the codec running is the worst case worth
  measuring.

#### 10.14.2 The library layer comes first, and knows nothing about approvals

Two layers, built in this order, and the order is the design:

1. **The library layer** — the board and the outside world as small,
   self-contained services: the I²C bus (below) and the chips on it (touch, RTC,
   PMIC, codec), the display transport, the JSON-backed settings of §10.15,
   Wi-Fi (§10.9), the NATS link (§10.5), crypto (§10.6). Each one is usable, and
   testable, without
   the rest.
2. **The logic** — the §7 flow, the screens of §10.8, registration (§10.7).

**Nothing in the library layer may know what an approval is.** No `key_id`, no
`behavior`, no `tool_input` below the line: the I²C driver does not know that a
touch will become a verdict. This is the same split the Python half already
has — `lib/` versus `approver/` in root §2 — and it is what lets the host tier
of §10.11 be the comprehensive one: a layer with no protocol in it is a layer
that runs under Unity on the `linux` target with a fake backend underneath.

#### 10.14.3 The I²C bus is shared, so it is leased

**Moved, with its number**: it is in [`hardware.md`](hardware.md), next to the
board and the drivers it binds.

#### 10.14.4 The house precedent — what is borrowed, and what is not

None of the above was invented here. The **house firmware** — a working C++
ESP-IDF product written by the same author, outside this repository, for a
different chip (esp32, 4 MB, an IDF 4.1-era manifest) — is where these
conventions come from, and the reason several decisions above are shaped the way
they are. Following that house style is deliberate: a second style in the same
pair of hands costs more than it buys. It is not a dependency and nothing here
reads from it; where a detail below cites "their" code, it is describing a
pattern, not a file to copy.

**Borrowed:**

- **The composition root** (§10.14.1) — their `MainFactory`, a `getInstance()`
  singleton holding every subsystem by value. This is the pattern, not just an
  example of it.
- **Class conventions**: copy constructor and assignment `= delete`d on anything
  that owns a resource, an `Init(...)` returning success kept separate from the
  constructor, and getters returning references.
- **Two levels for a bus**: one class owning the bus, one above it owning the
  devices and their "is it there" flags — §10.14.3's shape, their
  `I2CMaster`/`I2CHardware`.
- **Build shape**: a custom `partitions.csv` with `CONFIG_PARTITION_TABLE_CUSTOM`,
  the C++ standard set through `component_compile_options`, exceptions and RTTI
  off (they already are, in both projects), third-party trees vendored into
  `components/`, and `dependencies.lock` committed.

**Not borrowed, and each for a stated reason:**

- **Their flat `main/` with ~38 `.cpp` files side by side.** It has no seam
  between the board and the product, which is exactly the seam §10.14.2 requires
  and §10.11's host tier is built on. Our library layer lives in `components/`
  and `main/` stays thin.
- **`"*"` version ranges in `idf_component.yml`.** §10.4 pins.
- **`xTaskCreate`** — ours is `xTaskCreateStatic` with the stack as an array
  (§10.14.1).
- **The legacy I²C driver**, per §10.14.3 in [`hardware.md`](hardware.md).
- **A committed `sdkconfig`.** It is generated and target-specific; we commit
  `sdkconfig.defaults` and let the real one be produced by
  `idf.py set-target esp32c6`. Their `sdkconfig.ci` — a second configuration
  kept for CI — is worth remembering when there is CI.

**And the skeleton is generated, not copied.** `idf.py create-project`, then
`set-target esp32c6`, then `idf.py create-component` per library-layer piece.
What that produces is four files; adapting another chip's tree, with its
`sdkconfig` and its IDF 4.x assumptions, is more work than typing them and ends
somewhere less honest.

### 10.15 The configuration lives in JSON, and one button puts it back

**Everything this device is configured with is a JSON file in the `storage`
partition. NVS holds nothing of ours.** That is the decision; the rest of this
section is what it buys, what it costs, and the button that undoes it.

The files, built from `spiffs_image/` and flashed with the project (§10.12):

| File | Written by | Holds |
|------|-----------|-------|
| `config.json` | the firmware, whenever a setting changes | everything the operator can set: Wi-Fi networks **and their passwords**, the NATS URL, the `TZ` string and SNTP server, display timeouts, and the speaker's volume |
| `registration.json` | §10.7, once, on a verified `ok:true` | the registered `key_id` and the pinned handler `server_key` |
| `config.init.json` | **nobody, ever** | the factory defaults, and the only thing a restore has to copy from |

This is the house pattern of §10.14.4 — their `config.json` /
`defaultconfig.json` — with the defaults renamed to say what they are. Shipping
defaults *as a file* rather than as a `constexpr` struct is what makes a restore
one copy instead of a serializer that has to stay in step with the parser, and
it means the defaults can be read off a flashed device without a build.

**Why two written files rather than one.** The split is by *lifetime*, not by
secrecy: `config.json` is what the button restores, and `registration.json` is
what it must not touch. A device that comes back on default settings is a
minute's work; a device that comes back unregistered needs a new token minted on
the host and typed over USB (§6, §10.7). Same format, same filesystem, same
parser — one file is simply out of the blast radius.

#### What this gives up, stated plainly

The earlier draft of this section put Wi-Fi passwords and the pinned key in
**encrypted NVS**. That is now gone, and the honest accounting is:

- **SPIFFS cannot be encrypted at rest — at all.** ESP-IDF's flash encryption is
  implemented for FATFS and LittleFS; NVS has its own scheme; SPIFFS has
  neither. So the WPA password in `config.json` is readable by anyone who can
  run `esptool read_flash` against the `storage` partition. There is no
  Kconfig switch that fixes this.
- **What was given up is smaller than it sounds.** NVS is only encrypted when
  flash encryption is enabled and `nvs_keys` is populated — a one-way eFuse
  operation nobody has performed here (§10.12). Until that day, NVS and SPIFFS
  are equally readable, so the plan was promising a property it did not have.
  Trading a future property for one file, one parser and one restore is a
  defensible trade; pretending nothing was traded is not.
- **If encryption at rest is ever required, the filesystem changes, not the
  format.** `storage` becomes `fat` in `partitions.csv` with the `encrypted`
  flag, `spiffs_create_partition_image` becomes `fatfs_create_partition_image`,
  and the JSON is untouched. Doing it now would cost nothing; doing it after the
  first `config.json` ships costs a reflash of the partition. Worth deciding at
  the same time as the §10.12 encryption question rather than separately.

**And the signing key is in flash too, on the build that is running.** This
paragraph used to say the opposite — that the Ed25519 *private key* is derived per
boot from an eFuse key and exists only in RAM, so a dumped flash yields the
network password and the device's public identity and **not** the ability to sign
a decision. That is §10.6 **as designed**, and §10.6 shipped its *fallback*: no
eFuse key is burned, so a 32-byte seed sits unencrypted in NVS (the namespace two
sections down) and `esptool read_flash` gives up the key that signs verdicts.

So the honest accounting for a flash dump today is one sentence rather than two:
**everything on this device is readable — the WPA passphrase, the pinned handler
key, and the signing key.** §10.6's table has the row that says so and what closes
it (`espefuse.py burn-key`, after which the firmware picks the fuse up with no
reflash, deletes the stale seed, and becomes a different responder needing a new
token). This section is not what makes that true and cannot fix it: it would be
equally true with the config in encrypted NVS, because NVS encryption needs the
same one-way eFuse operation.

What §10.10 and §7 depend on is narrower than "the key cannot be read", and it
does still hold: a decision cannot be forged **by the host being asked about**,
and the only path to `allow` is a human press on a card. What a stolen or
flash-dumped device costs is that whoever holds it can sign as this responder
until `forget` and a re-registration take the `key_id` back — the same class of
thing §10.13 accepted when it decided there is no authentication on the device,
and one notch worse than it was written to be.

#### The `nvs` partitions, and the one namespace of ours that is in them

`nvs` stays in `partitions.csv` and is still initialised at boot: `esp_wifi`
requires `nvs_flash_init` for its own calibration and PHY data. `nvs_keys` stays
**reserved and empty** — 4 KB, and deleting it would shift every offset after it,
which is a reflash of a device that has already been registered.

**And the thing this section said "could still" put a namespace there has
happened**, which is why the heading changed: §10.6's fallback shipped, so there
is now exactly one namespace of ours in `nvs` — `approver`, holding a single
32-byte blob, the Ed25519 seed. The rule this breaks was "nothing of ours in
NVS", and the exception is argued rather than quietly taken:

- **it is not a setting, and every property of `config.json` is wrong for it.**
  §10.15's button must not restore it, `cat config.json` must not print it, and
  nobody should be able to edit it by hand. A separate store is the cheapest way
  to get all three, and it is the same argument that keeps `registration.json`
  out of `config.json` — the split is by lifetime, and a key's lifetime is not a
  setting's.
- **`nvs_keys` is still empty, and that is the disappointing half.** The whole
  point of putting a key in NVS rather than in a file was that NVS *can* be
  encrypted — but only with flash encryption burned, which is the one-way
  operation §10.12 has nobody performing yet. So the seed sits in plaintext, and
  until that changes it is exactly as readable as the WPA password two paragraphs
  up. §10.6's table has the row that says so.
- **so `nvs_keys` is now reserved for something specific rather than for
  something hypothetical**: populating it is what makes this namespace encrypted,
  and that is one decision — with the FATFS question above — rather than three.

#### The button

**`KEY`** — §10.1's free one, and the only one that is free.

- **Sampled early in boot, before the config is read.** The failure this button
  exists for is a config that stops the device booting; a restore that runs
  after the parse cannot rescue that. A GPIO read with no dependencies: before
  the filesystem, before the panel, before Wi-Fi.
- **Held ≥ 5 s → `config.init.json` is copied over `config.json`**, and boot
  continues on the defaults. Released early, nothing happens — there is no
  feedback that early in boot to make a partial press meaningful, which is the
  argument for a long threshold rather than a short one.
- **`registration.json` is not touched**, so the device comes back on default
  settings and still registered. Dropping the registration is `forget` on the
  console, and wiping both is this button followed by that command. Neither is a
  row on §10.8.5's list and both used to be: that list is the repository owner's
  now, and a screen entry that costs a token — one minted on the host and typed
  over USB — was not worth a place on it while the console has both verbs.
- **Say it happened.** The panel is not up that early, so: one log line at the
  time, and the screen states `config restored` as soon as there is a screen. A
  restore the operator cannot confirm is a restore they will do twice.

  Two things that only settled once it was on the glass. The line is under the
  date **for thirty seconds** and then goes — it was a minute, then five, and on
  the desk it read as a line that would not go away; the operator held a button
  through the boot and is looking at the device, so the window only has to
  outlast the glance. And what outlasts it is the console: `config` prints a
  `boot` line for the whole uptime, which is the same fact after the boot log has
  scrolled away and the screen has gone back to being a clock.
- **The copy is not allowed to half-happen.** Write `config.json.new`, then
  rename over `config.json`. A power cut mid-restore that leaves a truncated
  config would break exactly the recovery path being used. Every runtime write
  of `config.json` goes the same way, for the same reason.

  **SPIFFS does not implement that plan, and the difference is now handled
  rather than assumed.** `rename()` onto an existing name fails with EIO
  (errno 5) — measured on this board, and the first `play volume` refused to
  save because of it. SPIFFS renames only onto a free name, so the write is:
  temp file → `remove` the old → `rename`. That leaves a real window in which
  `config.json` is gone and a *complete* `config.json.new` is not yet called
  anything, so `config::Init` closes it before reading: both files present
  means the temp is a leftover and is dropped; only the temp present means the
  crash landed in the window and finishing the rename is the recovery — never
  restoring the defaults, which would throw away a good config to fix a naming
  problem.
- **There is no restore row on the settings screen**, and this paragraph used to
  say there was one. §10.8.5's list has `config save` and `config reload` — the
  two that are cheap and reversible — and stops there; a restore reached by a
  finger would be the destructive third, and the owner's list does not have it.
  So the two ways to restore are this button and `config restore` on the console,
  and the blind five seconds are the price of the one that needs no cable.

##### What is written, and the four decisions inside it

`config::RestoreAtBoot` in `components/config`, `board::InitButtons` next to it,
and three lines in `main.cpp` between `storage::Init()` and `config::Init()` —
which is the whole feature, because the button and the file both already existed
and what was missing was the moment between them.

**What the board has actually done**, three boots in a row: `KEY` held through
the reset, the restore logged at **5,001 ms** after the buttons came up,
`config.json` down from 992 bytes to the defaults' 824, the settings back to
`UTC` / radio off / no networks / 80 % — and `registration.json` still 133 bytes
with the same pinned handler key and the same registration date. Then `config
restored` on the glass under the date, and `config` on the console printing
`boot config restored (KEY was held)` for the rest of the uptime.

Four things are decisions rather than plumbing:

- **The button is not `config`'s to read**, which is why `RestoreAtBoot` takes a
  bool. A layer that knows about a file and its fields has never heard of a GPIO
  (§10.14.2), and `main` is where the two meet — the same place the codec's
  volume is applied. What that buys is the host tier: the whole of this is nine
  tests against a real filesystem, with no board and no fake button.
- **`board::InitButtons` exists because of the ordering**, not because the
  buttons wanted splitting. `board::Init()` cannot run this early — it brings up
  the I²C bus, and the panel's reset is a PMIC rail — so the one piece of this
  board that depends on nothing needed its own entry point. It is idempotent, and
  the guard is not tidiness: a second `Init` re-adopts the debounce state and
  would lose the press being held *right now*, which is the only press this
  feature is about.
- **A failure changes nothing on the filesystem, and that is a third outcome
  rather than a shade of one.** No `config.init.json`, or no mounted partition,
  and the settings that are there stay exactly where they are — destroying a
  working config to report a missing default file is the worst of both. So
  `RestoreOutcome` has three values and `BootRestoreText` two sentences, and the
  test that says they are different sentences is the one that keeps them so.
- **It is said in three places and they have three lifetimes.** The log line is
  the only record at the moment it happens, because there is no panel yet; the
  screen carries it for thirty seconds, which is the glance; `config` answers for
  the whole uptime, which is the boot log after it has scrolled away. §10.15 asks
  for the first two — the third is what makes "a restore the operator cannot
  confirm is a restore they will do twice" true an hour later as well.

And one thing only the board could have said. The console reported the notice was
up and the glass was empty: the label sat below its parent's box on the strength
of `LV_OBJ_FLAG_OVERFLOW_VISIBLE`, and that flag grows the parent's clip box by
`lv_obj_get_ext_draw_size(parent)` — **zero** unless a shadow or an outline
enlarged it, so a child entirely outside is clipped away. The flag makes an
overhang visible; it does not make a box bigger. The face now has two heights,
one to centre the clock on and one to contain the notice, and `clock_screen.h`
carries the finding.

#### Reading and writing it

- A `config.json` that is missing, oversized or unparseable is **restored
  automatically** from `config.init.json`, one log line, boot continues. Same
  call §10.10 makes about the bus: bad input is recovered from, never a reboot
  loop. A missing `config.init.json` is a *build* error — it ships in the
  image — not a runtime state to design around.
- A missing `registration.json` is not an error at all: it is the unregistered
  state, which §10.8.2 already requires the clock to announce.
- Parsing is cJSON into a fixed struct (§10.14.1: no heap in our code, one-shot
  setup paths may use what the libraries allocate). Cap the file size before
  parsing. **Unknown fields are ignored and lost on the next write** — a config
  written by a newer firmware does not survive a downgrade, which is the honest
  behaviour of a fixed struct and worth knowing before it surprises someone.
- **A password is a secret from the moment it is typed**, and the file being
  readable does not relax that: never logged, never in a console dump, never in
  a crash trace, and the §10.8.6 show/hide toggle stays a deliberate operator
  action.
- **What is committed carries placeholders.** `spiffs_image/config.json` is in
  git and now *is* the real store, which makes the temptation to leave a working
  WPA key in it much stronger than before. `CHANGEME` is what belongs there; a
  real key committed once is a real key in the history. **`nats.url` is the one
  deliberate exception** — a real address, committed, and argued below rather
  than left to look like an oversight.

#### What is written, and the shape the files now have

`components/config` is the component: `Init` at boot, `Reload`, `Save`,
`Restore`, and a `Data` struct the rest of the firmware reads fields off. It
replaced the placeholder schema the two files were carrying — an AP-mode Wi-Fi
block and a `WEB` section, both inherited from the house firmware of §10.14.4
and belonging to a device this one is not. What is in them now is what this
firmware has or is specified to have: the Wi-Fi block (§10.9 —
`active`, `mode`, `rounds`, `apWindowSeconds`, `ap.{ssid,password,channel}` and
`networks[]`, four of them; that section's table says what each is for),
`nats.url` (§10.3), `time.zone` / `time.posix` / `time.sntp` /
`time.syncHours` (§10.8.2), the display's brightness and the two idle
thresholds of §10.8.1 — `dimAfterSeconds`, `dimPercent`, `sleepAfterSeconds`,
which replaced a `dimSeconds` and a `blankSeconds` that nothing read —
`audio.volume` and
the `touch` block — the four numbers of §10.8.5's correction, **clamped on the
way in as well as refused at the fit**, because this file can be edited by hand
and a scale of 30000 typed into it is a screen nobody can press.

**`time.sntp` is the one string with no compiled-in default**, and §10.8.2 says
why: it names somebody else's machine, so an absent one means "do not sync"
rather than "sync against whatever this firmware was built believing". Empty
and absent are the same answer, and `time.syncHours: 0` is the third spelling
of it.

**`nats.url` is the string that goes the other way, and it deserves saying out
loud because it is committed twice over.** `config.init.json`, `config.json` and
`config::FillDefaults` all name `nats://192.168.11.70:4222` — this LAN's server,
in git, and in the binary. That is deliberate on the same reasoning that leaves
`sntp` empty, reaching the opposite answer: an NTP host is a stranger's machine
and the bus is the operator's own, so a restored device that connects beats one
that has to be told over USB where its bus is. What it costs is honest and
small — a private address is not a secret (§10.3 already put the bus on the LAN),
but it *is* a fact about one household baked into the defaults, so a second
device on a different network needs `nats url` and a `config save` rather than
just a flash. If this project ever ships to a second bus, the compiled-in default
is the line to empty, and "no server" already means "off" (§10.5) so nothing else
has to change.

`internet.targets` is the third field that names machines and it lands in neither
camp, which is what makes the pair above a judgement rather than a rule:
8.8.8.8 / 1.1.1.1 / 9.9.9.9 are strangers and they *do* have compiled-in
defaults, because an ICMP echo tells them nothing and there are three of them
precisely so that none is depended on. The question each of the three answers is
the same one — what does this device do to somebody else's machine, and what
breaks if that machine is not there — and it comes out differently for a clock,
a bus and a ping.

**An access-point block is back, and it is not the one that was deleted.** The
house firmware's was a device whose *normal* mode was to serve a web UI over
its own AP; this one is §10.9's fallback — up for two minutes at a time when
nothing else worked, so that a device on a desk with a wrong password is still
reachable. Same shape in the file, opposite reason for existing, and worth
knowing before somebody reads the two paragraphs as contradicting each other.

The server's address is `nats.url` rather than `bus.url` for the reason the
name suggests: there is one bus here and it is NATS, so an address reads as an
address instead of as an abstraction with a single implementation.

**`web.write` is the newest field and the only one that is a permission rather
than a setting** (§10.16): false makes the configuration site read-only — the pages
still draw, every form greys itself out and says why, and every `POST` answers 403.
True is the shipped value in both files, because a configuration site that cannot
configure anything is not what it was asked for; what it costs is that anybody who
can reach the server can submit the form, which is §10.3's trust boundary and not a
new one. It cannot be written *through* the form, which is a test.

**And who is told when a reload replaces every field at once is a hook, not a
list at each caller.** `config::OnChanged`, registered by `main`, called by
`Reload` and `Restore` themselves — the codec's volume, the panel's brightness,
the touch correction, the Wi-Fi manager's network list, the clock's sync interval,
the bus's URL, the web server's mode. It used to be a block at the bottom of the
console's `config reload`, which was correct while the console was the only
caller; there are three now (that command, the settings screen's row of §10.8.5,
and the boot restore), and three copies of that list is two of them going stale.
Two things came out of moving it: a reload from a finger and a reload from a cable
became the same reload, and `web::Apply` got called for the first time — `web.mode`
was read at boot and never again. **`Init` deliberately does not call it**: at boot
nothing it would tell exists yet, and `main` applies each of them explicitly in an
order that is written down there.

**Editing and persisting are two commands, deliberately.** `config set <field>
<value>` writes the field and nothing else — the settable ones are `volume`,
`brightness`, `dim`, `dimlevel`, `sleep`, `nats`, `tz`, `sntp`, `sync` and
`wifi`, and every one of
them says "in memory only" when it succeeds; `config save` is what reaches the
filesystem. (Three of those are §10.8.1's idle timer and they are the *new*
names: a `blank` here would be the field that nothing read, and
[`commands.md`](commands.md) is the reference with a row per field.) A console where each keystroke lands in flash is a console that
wears the partition out during an experiment, and `config reload` is then the
cheap undo for anything not saved. The Wi-Fi networks are the exception and are
not settable this way: they are a list of ssid/password pairs, and a list needs
add and remove rather than assignment — `wifi join <ssid> [password]` and
`wifi forget <ssid>` are those two verbs (§10.9), and they follow the same rule
to the letter: memory only, and `config save` writes. §10.8.6's screen is the
same pair of verbs with a keyboard in front of them.

**Four of them are applied as they are set** — `volume`, so the next `play` is
audibly the number just typed; `tz`, because the point of a zone is what `date`
prints; and `sync` and `sntp`, which reach the clock's sync task and *only*
that task. Which is the §10.9 lesson written down as a habit rather than as a
story about `wifi check`: reach for the narrowest thing that actually changed.
Being applied is not being saved — all four still say "in memory only", and
`config save` is still what writes.

`play volume <n>` is the same setter reached by a shorter name — it calls the
same function, so there is one behaviour rather than two commands differing in
whether they touch the filesystem. It did save, briefly; that was the
inconsistency this rule replaced.

**The volume is the first setting that round-trips**, and it is worth having as
the proof of the whole path: `play volume 45` writes the field and the file,
the codec follows immediately, and after a hard reset the boot sound comes back
at 45 % because `main` applies `config.audio.volume` to the codec. `play` with
no arguments uses the *file's* volume rather than whatever the codec was last
set to — the file is the record of what the operator chose.

**The brightness is the second, and for a while it only looked like one.** The
field was there, `config set brightness` wrote it, `config save` put it in the
file — and `main` never handed it to the panel, which came up at whatever
`Panel::Init` left it at. A setting that survives a reboot and changes nothing
is worse than one that is missing: there is nothing for the operator to doubt.
It is applied now, before the splash, so the first thing on the glass is
already at the brightness that was asked for.

**What found it is the reading form of `display brightness`** — and that is the
argument for every `[0..100]` in §10.7 being optional rather than required.
The panel's live value and the stored one are two numbers; the read prints them
side by side (`brightness 100% (config says 80%)`), and nothing else on this
device does. The command had required its argument, so the documented spelling
was an error — and the usage text it printed then said `<0..100>`, so the
console and the docs disagreed about which of them was wrong. Fixed in the
direction the docs had it, because `play volume` was already that shape and one
of the two had to be the rule.

Where that application happens is deliberate: `main` reads the config and sets
the codec, because `config` knows nothing about a codec and `board` knows
nothing about a file (§10.14.2). It is also why boot order changed — storage
and the settings on it now come up **before** the hardware they configure.

#### Tests (§10.11, host tier — none of this needs a board)

**The button included**, which is the part that sounds as though it needs one and
does not: `RestoreAtBoot` takes whether it was held rather than reading it, so
"released early, nothing happens" is a test that the file came out byte-identical
rather than a thing somebody has to go and hold. Nine of them, five mutations,
and §10.11 has what the two awkward ones taught.

Defaults parse; every field missing; a truncated file and a non-JSON file both
ending in a restore; a restore leaving `registration.json` untouched; `forget`
removing it; and `config.json` and `config.init.json` having the same shape —
the last one is the test that catches the two drifting apart, and the Wi-Fi
block of §10.9 was added to it as the newest half of the file and therefore the
likeliest to be put in one file and forgotten in the other.

Four came with that block: the settings read back as written; an unknown
`wifi.mode` keeping the default rather than being guessed at; the access point
surviving a `Save`/`Reload` round trip; and **a `config.json` of `{}` still
leaving an SSID to raise** — the fallback AP is what rescues a device that
cannot reach a network, so it must not itself depend on the file being complete.

Nine more came with the fixed address of §10.9, and the parser is where most of
the value is: the dotted quad with the **first octet in the low byte** (get
that backwards and you have a plausible address on a network nobody is on), a
leading zero refused rather than read as octal, trailing junk and an octet over
255 refused, DNS optional, a broken static block falling back to DHCP **with
the SSID kept**, a bad DNS entry costing only that entry, the whole thing
round-tripping through `Save`/`Reload`, and a network on DHCP writing no `ip`
block at all. All seven mutations of that logic were caught.

Three more that came out of asking what an *edited* file can contain, since
this one is meant to be edited by hand:

- **valid JSON that is not an object** — `[]`, `42`, `"hello"` — is restored
  rather than read as an object with no fields in it. That was a real hole:
  every lookup answered null, the device came up on the defaults saying
  nothing, and the file stayed to do it again next boot;
- a string longer than its field is **refused, not truncated** (`CopyString`
  says why: a half-length SSID fails to connect and gives no hint which half
  is being used), and a network whose SSID was refused is dropped rather than
  kept with an empty one;
- a negative number is **clamped, not wrapped** — without the clamp `-5`
  reaches `uint8_t` as 251 and `-1` reaches `uint16_t` as 65535, which is a
  brightness and a blank timeout that both look deliberate.

