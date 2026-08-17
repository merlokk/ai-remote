# The console — every command, and what it does

The device answers on its USB-C port: it is the ESP32-C6's own USB Serial/JTAG,
so the port appears only while the board is powered, and it is the same port a
monitor uses. **How to open it** — the monitor command, the pyserial snippet for
driving it from a script, and what to do when something else is holding the
port — is in [`working-with-code.md`](working-with-code.md). **Why the console
exists and why each command behaves the way it does** is in
[`CLAUDE.md`](CLAUDE.md) §10.7. This file is the reference: what you can type,
and what happens.

`help` lists everything; `<command> help` prints the forms where a command has
more than a couple.

## Two rules that apply almost everywhere

**Setting something and saving it are two commands.** Everything that changes a
setting says *in memory only* when it succeeds, and `config save` is what
reaches the filesystem. A console where each keystroke lands in flash is one
that wears the partition out during an experiment — and `config reload` is then
the cheap undo for anything not saved.

**A password is never printed.** Not by `config`, not by `wifi`, not in a log
line. Addresses, SSIDs and channels are not secrets and are printed in full.

---

## What the device is

### `devstatus`
Everything at once: the board, every chip on it, the screens, the clock, the
network and the bus — `status`, `power`, `buttons`, `imu`, `audio`, `display`,
`clock`, `request`, `date`, `wifi` and `nats`, one after another under `== name`
headers.

**Each header is the name of the command that prints that section on its own**,
so the dump doubles as a map: something odd under `== power`, type `power` to
look at it by itself. It runs the real commands rather than reprinting what
they print, so it cannot drift from them.

State, not settings — `config` is the other half and prints what the file says.

### `status`
Firmware name, version and build date; the ESP-IDF version it was built with;
chip model, revision and core count; the MAC; **which OTA slot is running** and
where; uptime; free heap and the lowest it has ever been; and the storage
partition's usage.

The low-water heap figure is the one that says whether the device is safe — the
current free heap only describes this instant.

### `power`
The AXP2101: charge state, whether VBUS is present and at what voltage, battery
millivolts and percentage, the system rail, die temperature, the power-button
thresholds this board is actually configured with, why the chip last powered on,
and the state of the DCDC1 / ALDO2 / ALDO3 rails.

### `reboot`
Restarts the device. **Anything set and not saved is lost** — `config set`
writes to memory and `config save` is what reaches the filesystem, so this is
where unsaved edits go; the command says so before it goes.

No confirmation word, unlike `poweroff now`, and the difference is what each
one costs: a power-off needs somebody to walk over and press a button, a reboot
undoes itself in a few seconds. The console comes back on the same port after
the boot; `status` shows the uptime starting again.

### `poweroff now`
Cuts power. **Refused while USB is connected**, and nothing is written when it
refuses: VBUS is a power-on source in its own right, so a shutdown with the
cable in is one the hardware immediately undoes — what you would see is not a
device switching off but a device rebooting. Unplug first. The confirmation word
`now` is required.

---

## The hardware, one command per chip

### `buttons`
BOOT, KEY and PWR: the debounced state, **the raw pin beside it**, and how long
each has been that way. The two answers disagree exactly when something is
wrong — a pin held low by a fault reads pressed in both, for the whole uptime.

### `buttons watch [seconds]`
Prints edges as they happen, with the duration of the press each one ends.
Default 10 seconds, capped at 120. Blocks the prompt while it runs.

### `imu`
The QMI8658C: all six axes, tilt, die temperature — and **a magnitude line**,
which is the point of the command. Six plausible numbers say nothing on their
own; at rest the acceleration vector must be 1 g, so `magnitude 0.964 g (1.000
at rest)` is the single line that says the other six mean something.

Nothing in the approval path may read the IMU. A tilt is not a press.

### `imu watch [seconds]`
A line a second of the same six numbers. Default 10 seconds, capped at 120.

### `date`
The RTC and the system clock in UTC, and the same instant in the configured
zone with its offset and whether summer time is in force.

A clock that was never set prints `--:--`-style dashes rather than a plausible
wrong time: the chip's oscillator-stopped flag makes "the read succeeded" and
"the time can be trusted" two different answers.

Then a `sync` block, which answers a question a clock face cannot: where this
time came from.

```
sync       pool.ntp.org every 6 h
           last 2026-08-16 22:35:44 Europe/Kyiv, 9 s ago, moved the clock +2 s
           next in 5h 59m
```

The middle line is printed **whether or not syncing is switched on** — when it
last synced is a fact about the time above rather than about the schedule — and
it is in the configured zone, like the `local` line, because that is how people
ask the question. `never synced` when it has not.

**How far it moved the clock** is the number worth reading: a device stepped by
several seconds every time has an RTC to be suspicious of, and nothing else
here would say so. A `+1` or `+2` on the first sync after a reboot is normal
arithmetic rather than drift — the RTC keeps whole seconds, so the clock
adopted at boot always starts a fraction of a second behind.

The last line is the schedule, and it is left out when syncing is off. `no
internet to ask through` is what it says instead of a countdown when there is
nothing to ask.

### `date sync`
Asks the time server now instead of at the next interval, waits for the answer,
and prints the same `sync` block. Refused, with the reason, when syncing is off
or there is no internet — a device that cannot ask should say so rather than
appear to have asked.

It does not need the RTC: SNTP sets the system clock whether or not there is a
chip to store it in, and a board whose RTC did not answer is the one that needs
the network's time most.

### `date set <YYYY-MM-DD> <HH:MM:SS>`
Writes both the RTC and the system clock. The time is read as **local** — what
is on a wall or a phone — and stored as UTC.

### `date set utc <YYYY-MM-DD> <HH:MM:SS>`
The same, taking the time as UTC already. The escape hatch for when the zone is
wrong and the clock still has to be right.

### `display`
Whether the panel is there and lit, the LVGL state, the touch controller — and
**missed reads**, a counter that climbs when the touch driver could not get the
I²C bus. No other readout shows that.

### `display on` / `display off`
Blanks the panel without dimming it, and brings it back.

### `display brightness`
Prints the panel's **live** value and what the stored setting says, side by
side: `brightness 100% (config says 80%)`. Nothing else on the device prints
both, and the gap between them is how a setting that never got applied is
found.

### `display brightness <0..100>`
Sets the panel only, right now. `config set brightness` is the stored one.

### `clock`
What the clock screen is showing, and **why** — the one readout whose subject is
already visible, which is exactly why it earns its place: a screen is the only
output of this device that cannot be captured from a script, and an icon can show
a colour without being able to say what made it that colour.

    face       14:12
    drift      -21,-33 px of +-30,+-40
    water      phase 107 of 256, 6400 ms per cycle
    wifi       off - hollow bars, 0 of 3 bar(s) lit
    bus        red - there is a server and we are not on it
    battery    cable in, not taking current, 100%
    updates    147, 0 gave the frame up waiting for the display
    stack      2968 byte(s) never used, of 4096

`face` is `--:--` when no believable time has arrived yet — the screen shows
dashes rather than a plausible midnight, and this is where the reason is written
out. **`drift` and `water` both move on their own**, so two runs a second apart
are the check that the panel is being looked after: the face wanders around a
box so no pixel carries an edge for long, and the digits are filled from a
travelling wave rather than a flat green.

The three icon lines say which of each indicator's states is on screen, in
words. `updates` next to the count of frames given up waiting for the display is
the load figure: a number that climbs there means something else is holding
LVGL — and `screenshot` below is the one thing that reliably makes it climb.

### `screenshot`
The frame itself, as base64, for `tools/screenshot.py` to turn into a PNG.
Nothing here is meant to be read by a person.

**It holds up the display while it runs** — a few seconds, because 460 800 bytes
of RGB565 is 614 400 characters. That is deliberate and it is why `clock` shows
a handful of frames given up afterwards: the screen task could not have the
display while the transfer had it.

Why it streams instead of taking a picture and handing it over: the panel cannot
be read back over QSPI, and a whole frame does not fit in this chip's RAM
alongside the network stack. So the pixels are sent a rendered strip at a time
and forgotten, and the host reassembles them.

    screenshot
    -----BEGIN SCREENSHOT 480 480 rgb565le-----
    @ 0 0 479 39
    <base64>
    …
    -----END SCREENSHOT 12 460800-----

Each `@` line is one strip's rectangle, so a strip lands where it belongs rather
than wherever concatenation would put it. The closing marker carries the strip
count and the byte count, and the decoder refuses a capture that does not match
them — a truncated transfer fails loudly instead of producing half a picture.

**The pixel order is the little-endian RGB565 LVGL renders**, not the big-endian
this panel is fed: the swap happens after the pixels are handed over. Nothing to
work around, and worth knowing before decoding it by hand.

Taking one is a single command on the host — [`working-with-code.md`](working-with-code.md)
has it.

### `request`
The permission card (§10.8.4) — what is on it, and the tally.

    card       Bash, 116 s left
    cwd        E:\projects\ai-remote
    input      {"command": "rm -rf build"}
    waiting    1 more
    last       allowed - Bash
    tally      1 allowed, 0 denied, 1 timed out
    guards     0 refused, 0 press(es) ignored
    answering  yes - listening on approvals.* in the group approvers
    wire       4 arrived, 3 shown, 1 dropped
    sent       1 repl(ies): 1 allow, 0 deny
    unanswered 0 press(es) that never reached the bus
    stack      6024 byte(s) never used, of 12288

`input` is a **preview** and says how many bytes it is not showing. The screen is
where a command is read in full: a console line that looked complete would be the
truncation §10.8.4 forbids, arriving through the back door.

`guards` counts two things that are the design working rather than faults —
`refused` is a request this device would not put in front of a human (the queue
was full, a field did not fit, or there was nowhere to answer into), and `ignored`
is a press that began before the card appeared or inside its first 300 ms.

**`answering` is the line to read first**, because a device showing requests and a
device answering them look identical from the glass. It is `yes` only when all
three are true — there is a key (`keys`), there is a registration (`register`) and
there is a bus connection (`nats`) — and when it is `no` it names which one is
missing, because each has its own command to go and look at.

That is not caution, it is how the subject works: each request reaches exactly
**one** responder in the group, so a device that cannot sign would take requests
away from one that can and answer them with silence. Not being there is the better
failure.

`wire` is what came off the subject: how many arrived, how many became a card, and
how many were dropped — a payload that would not parse, a field longer than this
device will show, a request with nowhere to answer into, or a card queue that was
full. Every drop is a log line as well.

`unanswered` counts the thing worth watching: **presses a human made that nobody
heard.** Reaching the bus can fail after the button — the signature, the publish, a
socket that went and came back between the press and the reply, or more decisions
at once than there were slots. It is the fail-safe working, and it is still the
number that says somebody answered something Claude Code never learned about.

### `request test [seconds]`
### `request test <tool> <text …>`
Puts a synthetic card up — 30 s by default, or the seconds you name, or a tool and
arguments of your own. It plays the alert and prints which button does what.

**It is no longer the only way to raise a card** — the bus is the other one — and
it stays because it works when the bus does not: no network, no registration, or
simply nothing asking. A card from here is indistinguishable to everything above
it, deliberately, which is what makes it worth testing with.

The one difference is where the answer goes: a card raised here carries no reply
subject a hook is waiting on, so answering it signs and publishes into a subject
nobody is reading. `sent` goes up and no one hears it.

The card is answered **on the board**: `BOOT` allows, `PWR` denies. There is no
console command for either, and there will not be — the only path to an allow is a
human press on a card that is on the screen.

Two things about `PWR` worth knowing. With no card up it is the way back to the
clock. And holding it for six seconds powers the board off, which is the power
chip's own behaviour and happens whatever the firmware thinks.

### `audio`
The ES8311 and the I²S channel in front of it: whether the codec answered at
boot, the volume, whether it is muted, and the channel's sample rate. **Muted
is the resting state, not a fault** — the codec's one job is a chirp on a new
request, and `play` unmutes around the file, so a codec found unmuted here is
the odd one.

### `play [file]`
Plays a WAV from the storage partition; with no argument, `alert.wav`. The
firmware has no decoder — uncompressed PCM only, and a file it cannot play is
named as such rather than played as noise. Blocks for the length of the file.

### `play volume`
Prints the codec's live volume and what the settings say.

### `play volume <0..100>`
Sets it, applied immediately so the next `play` is audibly the number just
typed. The same setter as `config set volume`, reached by a shorter name — in
memory only.

---

## Settings

### `config`
Prints everything parsed out of `config.json`: Wi-Fi mode and networks, the
fallback access point, the internet check, the NATS URL, the time zone and
SNTP server, display timeouts and the volume. Passwords show as *set* or *not
set*, never in full.

`config help` prints the forms, the way `wifi help` and `nats help` do, and it is
also what anything unrecognised prints. The settable field names live in exactly
one string in the source, so the usage block and the setter's own "unknown field"
line cannot drift apart.

### `config reload`
Re-reads the file, discarding anything changed since. Deliberately does **not**
restore defaults on a bad file: a reload is somebody asking what the file says,
and answering by overwriting it would destroy the thing they were asking about.

### `config save`
Writes the current settings back, atomically.

### `config restore`
Puts the factory defaults (`config.init.json`) back over `config.json`. The
registration is not touched.

### `config set <field> <value>`
In memory only. The settable fields:

| Field | Takes | Notes |
|---|---|---|
| `volume` | 0..100 | applied to the codec at once |
| `brightness` | 0..100 | stored; `display brightness` is the live one |
| `dim` | seconds | idle before dimming, 0 disables |
| `blank` | seconds | idle before the panel blanks, 0 disables |
| `nats` | URL | the bus address; **parsed before it is stored**, and applied at once |
| `tz` | zone name or POSIX rule | applied at once; see `config zones` |
| `sntp` | hostname | **empty means the clock does not sync** |
| `sync` | hours | how often the clock is corrected; **0 is off** |
| `wifi` | `on` / `off` | the radio switch |

A `tz` that is neither a known zone name nor a POSIX rule is **refused** — libc
reads a misspelled zone as UTC and says nothing, so a typo would silently move
the clock.

`sync` is the gap between *scheduled* corrections and not the whole rule: a
fresh boot and an internet that has just come back both sync at once, whatever
it says. Setting either `sync` or `sntp` takes effect immediately and touches
nothing else — in particular it does not disturb the connection.

The Wi-Fi networks and the ping targets are lists rather than single values and
have their own verbs (`wifi join`, `wifi check`).

### `config zones [filter]`
The time zones this firmware knows by name, optionally filtered —
`config zones Europe` finds the spelling without scrolling past Australia. Any
POSIX rule is accepted by `config set tz` as well.

---

## Wi-Fi

`wifi help` prints the forms. Nothing here writes the settings file; `config
save` does.

### `wifi`
The status, and it answers two different questions at once:

- **wanted** — what the settings ask for (off, client, ap);
- **state** — what is actually happening on the way there: connecting,
  waiting with the delay before the next attempt, connected, the temporary
  access point with its countdown;
- **radio** — mode, link, SSID, RSSI, **the channel actually in use**, and the
  address with `(static)` or `(dhcp)` beside it;
- **internet** — online / offline / unknown, how long since anything answered,
  failed rounds, when the next check is due, and which addresses are pinged;
- the remembered networks, with any fixed address, which one is current, and
  any whose password was refused;
- the fallback access point, when it goes up, and **whether it is protected**.

That last one is a setting rather than a property of the firmware: the AP this
device raises is open or WPA2 depending on `wifi.ap.password` in
`config.json` — empty is open, eight characters or more is WPA2, and anything
between one and seven is refused so that the AP does not come up open while
somebody believes it is locked.

`wifi` prints `open` or `wpa2` next to the fallback AP — read off the settings,
not off the air. So a password of one to seven characters shows up as an access
point that never appears at all, with the reason in the log, rather than as one
that quietly comes up open.

### `wifi mode off|client|ap`
What the radio should be doing. `off` means the radio is down and its stack is
released; `ap` is a permanent access point; `client` joins the remembered
networks in turn.

### `wifi join <ssid> [password]`
Remembers a network — replacing an entry with the same SSID rather than
duplicating it — switches to client mode and tries it now. Up to four networks;
no password means an open one.

### `wifi forget <ssid>`
Drops a remembered network.

### `wifi static <n> <address> <netmask> <gateway> [dns1] [dns2]`
Gives network `n` (as numbered by `wifi`) a fixed address. Every field is
parsed before any of it is stored, and **a leading zero is refused** rather
than read as octal: `010` is ten to whoever typed it and eight to most C
libraries. DNS entries are optional.

### `wifi static <n> off`
That network goes back to DHCP. The typed address is kept, so turning it on
again does not mean typing it again.

### `wifi scan`
What is on the air, strongest first, with channel, signal and whether it is
locked. **Works with the radio off** — it brings the station up for the scan and
puts it back. 2.4 GHz only: the ESP32-C6 has no 5 GHz radio, so a 5 GHz-only
network is not missing from this list, it is inaudible.

### `wifi ping`
Runs the internet check now instead of waiting for the next interval, and
prints the status when the round finishes.

### `wifi check`
Prints the internet check's settings: on or off, the interval, the timeout, how
many failed rounds mean offline, and the addresses.

### `wifi check on|off`
Stops or starts asking. Does not touch the connection.

### `wifi check <address> [address …]`
Replaces the list of addresses to ping, up to four, and starts a round at once.
They are pinged, so **a hostname is refused** — there is no name resolution in
an ICMP echo, and a target that can never answer would read as an outage that
never ends.

### `wifi retry`
Starts the connection cycle again from the top, clearing the round counter and
any remembered password refusals.

---

## The bus

`nats help` prints the forms. Nothing here writes the settings file; `config
save` does.

The device connects to the NATS server named by `nats.url` as soon as there is
a **client link with an address** — not an internet: the server is on the LAN,
so a router whose uplink is down is a perfectly good place for this to work.
Being an access point is not a network for this purpose; there is no route to
the bus from a device that *is* the network.

### `nats`
Where the bus is and what the link is doing:

- **wanted** — connected or disconnected, and the address the settings name;
- **state** — off / no network / connecting / waiting with the delay before the
  next attempt / connected with how long it has been up;
- **server** — the host and port the URL parsed to, or a note that there is
  nothing to connect to;
- **network** — whether there is a client link to go through;
- **last error**, when the last attempt failed;
- **history** — connects, drops, and refusals since the last success;
- **traffic** — messages and bytes each way;
- **stack** — the lowest the bus task's free stack has ever been. It is on show
  because the number it is watching was chosen after a stack overflow: the
  client library reserves 4 KB of frame in one function, and the 4 KB every
  other task on this device gets was not enough;
- the subscriptions, with their queue group and sid.

An empty `nats.url` is "off" — one switch rather than two fields that can
disagree, the same call an empty `sntp` server makes about the clock.

### `nats connect`
Try now, without waiting out the backoff, and switch the link back on if
`nats disconnect` had switched it off.

### `nats disconnect`
Drop the connection and stay off until `nats connect` or a reboot. It is an
override, not a setting: nothing is written and a reboot comes back connected.

### `nats retry`
Drop whatever is up and start again from the top, whether or not anything is
wrong with it.

### `nats url <nats://host[:port]>`
Point the device at a different server. In memory only.

The address is **parsed before it is stored**, and the refusals are deliberate:
`nats://host:4222`, `host:4222`, `nats://host` and a bare `host` are the four
spellings accepted, the port defaults to 4222, and a path, credentials, a
bracketed IPv6 literal, surrounding space, a port outside 1..65535 and the
`ws://` / `wss://` / `tls://` schemes are each refused with the reason. Those
last three name transports this firmware does not speak; accepted quietly they
would be a device that never connects and never says why.

A changed address drops the connection that is up — it is to the wrong server —
and reconnects at once. An unchanged one costs nothing.

### `nats sub <subject> [queue group]`
Watch a subject; anything that arrives prints itself, with its size, its
reply-to subject when it has one, and up to 240 bytes of payload with
non-printable characters shown as dots. Everything on that bus is untrusted
input, so the bound and the dots are the point.

The queue group is an argument rather than a flag because the protocol makes it
one: `approvals.*` in the group `approvers` is a different subscription from
`approvals.*` on its own, and the difference is who else gets the message.

Needs a connection — `SUB` is a line on the wire. Subscriptions survive a
reconnect; the client restores them.

### `nats unsub <subject>`
Stop watching one.

### `nats pub <subject> [text …]`
Publish, then **wait for the server to confirm it**. Publishing is not
delivery: the flush is what says the bytes arrived, and on a console — where a
person is waiting — that is worth the two seconds. The words after the subject
are joined with single spaces, and a payload too long for the command's buffer
is refused rather than truncated.

---

## The key

### `keys`
This device's identity on the bus, and whether it can prove it:

```
key id     approver-esp32
key type   ed25519
state      ready
source     a saved key, readable from a flash dump
public key dFEabltdxgbWgh3CE5XL7ul7oMAAtc248oYeAKfJyzA=
            this is the fallback: the key is saved on the device and a flash
            dump gives it away. burning a key into the chip is what fixes it,
            and it changes this device's identity when it happens.
registered no - registration is not implemented yet
```

**`source` is the line to read**, because there are two ways this device can
have a key and only one of them is the design:

| `source` | What it means |
|---|---|
| `derived from a chip key, nothing stored (chip key block N)` | the intended one. The key is recomputed at every boot from a key in an eFuse block the software cannot read; nothing holding it is written anywhere, and a flash dump gives up the public half only |
| `a saved key, readable from a flash dump` | **what is running today.** No chip key is burned, so a seed was generated once and kept on the device. `esptool read_flash` gives up the signing key |

The device does not choose between them at runtime: a chip key wins whenever
one is burned. Burning one later needs no new firmware — and it **changes this
device's identity**, so the registration goes with it.

`state` has three ways of not being `ready`, and each needs something different:

| `state` | What to do about it |
|---|---|
| `ready` | nothing |
| `self-test failed - the signature would not be trusted` | a build problem, not a board problem. Do not use this firmware to approve anything |
| `the chip refused to use its key` | a key block is burned but not for this purpose, or the hardware is faulty |
| `no key, and none could be saved - it cannot sign` | the seed could not be written. A device whose key changed every boot would register once and be rejected afterwards, so it refuses instead |

Anything but `ready` means this device will never publish a decision — no
reply, the hook times out, and Claude Code asks in its own terminal. That is
the intended failure, not a fault to work around.

**`public key` is the string to compare by eye**, once, against what the
registration handler prints at startup. It is the public half; printing it
gives nothing away.

### `keys selftest`
Two checks, and they answer different questions:

```
library    passed - signatures match the host's
this key   passed - it signs and its own public key verifies it
message    approver-esp32 key check v1
signature  Ryhn2qqYvXjeG6YZKi48sra1zXXcaQcIlcEv08uiyR6p78S++Y355v+AJ2lQ…
public key dFEabltdxgbWgh3CE5XL7ul7oMAAtc248oYeAKfJyzA=
```

- **`library`** is what runs at boot before the key is derived at all: a fixed
  key signs a fixed message, and the result is compared against bytes the Python
  side produced — in both directions, a signature this device made and a
  signature it has to accept, plus one with a flipped bit that it must reject.
- **`this key`** is the other half. The library being right says nothing about
  the key *this* device derived, and a keypair whose halves do not match signs
  perfectly happily and verifies against nothing.

The three strings underneath are there so the check can be finished on the host:
paste them into `lib/crypto.py`'s verify and it should say `True`.
`working-with-code.md` has that line.

It is **not** a way to get arbitrary bytes signed. The message is a constant with
no argument, and it deliberately begins with a letter — the bytes a decision is
signed over always begin with the protocol version and a newline, so nothing this
prints can be rearranged into a verdict.

### `keys forget now`
Deletes the saved key. Refuses without the word `now`, for the reason the
destructive settings entries do: what is lost is a registration, and getting it
back means a fresh token minted on the host.

The current session keeps signing with the key it already has — nothing breaks
mid-session. The next boot makes a new one, and this device is then a different
responder.

Nothing to forget is not an error: a device running on a chip key has no saved
seed, and says so by succeeding quietly.

### `register <token>`
The exchange of §6, over USB because the token is ~50 characters of base64 that
must be transcribed exactly and a 2.16-inch touchscreen is a bad joke:

```
register approver-esp32.4+NmtVFJVG8nGwWHGXj8TRbG8b8WntOQonP4W950bEA=
registering as approver-esp32...
registered as approver-esp32, handler key Q15MkgcK2zYbfbLfxF2CFN8jyRRluEjWxbHnhRL1Zv0=

check that handler key against what the handler printed at startup. it is
pinned now: a reply signed by any other key will be refused from here on.
```

The token is minted on the host — `working-with-code.md` has the command — and
is **one-time**: a token that has been used, or has expired, is refused.

**Compare the handler key, once.** The handler prints the same string when it
starts. That comparison is the whole of trust on first use: after it the key is
pinned, and a reply signed by anything else is refused rather than believed.

What it needs, and it says which is missing rather than failing vaguely: a token
that is one, a key of its own (`keys`), and a bus connection (`nats`). The token
is checked first, because a mistyped one is wrong whatever the network is doing.

Every failure leaves the device exactly as it was — a rejected registration
cannot clobber one that works:

| What it says | What happened |
|---|---|
| `that is not a token: it must be '<key id>.<secret>'` | nothing left the device |
| `that token is for 'x'; this device is 'approver-esp32'` | the handler would refuse it too, several seconds later and over the network |
| `not connected to the bus - 'nats' says why` | there is nobody to ask |
| `this device has no key to register: …` | see `keys` |
| `the handler refused: token unknown` | a **signed** answer from the handler: the token is spent, expired, or was never minted. Mint another |
| `the answer could not be trusted: signed by a different key than this device already trusts` | somebody answered who is not the handler this device is pinned to. This is not a registration problem |
| `no answer in 10 seconds - is the registration handler running?` | the bus is up and nothing is listening on `registrations` |

That second-to-last row is the reason the reply is verified before it is read at
all: `registrations` is an open subject, so anyone on the network can answer, and
an unsigned "expired" would send you hunting a problem that does not exist.

### `forget now`
Drops the registration and the pinned handler key. Refuses without the word
`now`, and says what it will cost first: the handler still lists a key for
`approver-esp32` that this device can no longer use, so **a new token has to be
minted** before it can register again.

The signing key is untouched — that is `keys forget now`, which is a different
thing and costs a different thing. This one costs a token; that one costs the
identity.

Nothing to forget is not an error, it just says so.

### `limits`
The last `status` document a Claude Code session published, and whether the
screen for it is up:

```
watching   yes - status
documents  14 arrived, 0 unreadable
screen     up
last       3 s ago
model      Opus 5 (1M context), effort high
session    E:\projects\ai-remote\approver-esp32
5h         61% spent (yellow), resets in 1h4m
7d         76% spent (yellow), resets in 1d6h
ctx        72% spent (red)
```

**This screen arrives; you do not navigate to it.** A document lands and the
screen comes up; a minute with none and it goes back to the clock; `PWR` sends it
back now. The status line publishes on every render, so in practice the screen is
up while you are working and the clock is up when you are not.

`PWR` dismisses the **burst**, not the message — the next document is a few
seconds away, so a back that lasted one message would be undone before your finger
left the button. The screen stays away until the stream stops and starts again.

Two things the readout is honest about that the screen alone could not be:

- **`last … s ago`**, and `the stream has stopped` once the minute is up. These
  numbers are a current value with nothing behind them: they are as true as they
  are recent, and the last good document is kept rather than blanked.
- **`not published`** against a gauge, which is not the same as 0 %. An API key
  rather than a subscription has no rate limits at all, and an empty bar would say
  the opposite.

The colours are the status line's own scales, and they differ per gauge on
purpose: a rate-limit window is green to 50 % and yellow to 80 %, the context
window green to 20 % and yellow to 45 %. Half a five-hour window is an ordinary
working state; half a context window is most of the way to a compact.

Nothing here can be acted on, and that is the design: deleting this screen has to
leave a working responder, so it has no way to reach one.

---

## The filesystem

### `ls`
Everything in the storage partition with sizes, plus how much of the partition
is used. SPIFFS is flat, so this is the whole filesystem rather than one level
of it. Sixteen entries at most, and it says so rather than looking complete.

### `cat <path>`
Prints a file. Reads through a fixed 4 KB buffer, and a file too big for it is
refused **with its size** rather than truncated into something that reads as
complete.

---

## The terminal itself

### `term`
Asks the terminal to identify itself and turns on line editing and up-arrow
history if it answers. Needed because the probe that would do this runs while
the console is being created, and on USB Serial/JTAG nobody is attached that
early.

**Never type this in a scripted session.** With line editing on, the editor
asks for the cursor position before every prompt and blocks waiting for the
answer; a script that does not reply leaves the console silent until the board
is reset. If it happens, send `\x1b[24;80R` and then `term dumb`.

### `term smart` / `term dumb`
Forces the mode when the probe's answer is wrong.

---

## What the console owes the protocol

All four of the commands this section used to list as missing exist now.
`register <token>`, `keys` and `forget now` are above; `bus <url>` is **`nats
url <url>`**, alongside a status readout, `connect` / `disconnect` / `retry` and
`sub` / `pub` for looking at the wire — for the reason `wifi` grew subcommands
rather than staying a bare pair of words.

`forget` turned into two commands rather than one, because it was specified to
drop two things that cost different amounts: **`forget now`** drops the
registration and the pinned handler key, and **`keys forget now`** drops the key
this device signs with.

Nothing is missing any more: the device subscribes to `approvals.*`, shows what
arrives, and signs and publishes what a human presses. `request` is where you look
to see whether it is doing that, and which of the three preconditions is missing
if it is not.

`wifi <ssid> <password>` was once specified as the headless way to join a
network. It is `wifi join <ssid> [password]` now: `wifi` grew a status readout
and subcommands, and a bare pair of words that is sometimes an SSID and
sometimes a subcommand is a parser with a trap in it.
