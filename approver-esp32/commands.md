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
Everything at once: the board, every chip on it, the clock, the network and the
bus — `status`, `power`, `buttons`, `imu`, `audio`, `display`, `date`, `wifi`
and `nats`, one after another under `== name` headers.

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

## Not built yet

These are the commands the approval protocol will need. They are specified but
do not exist:

| Command | Will do |
|---|---|
| `register <token>` | the registration exchange with the handler — the reason this console exists |
| `keys` | print this device's public key and the pinned handler key |
| `forget` | drop the registration and the pinned key |

`bus <url>` was the fourth of these. It exists as **`nats url <url>`** —
alongside a status readout, `connect` / `disconnect` / `retry`, and `sub` /
`pub` for looking at the wire — for the reason `wifi` grew subcommands rather
than staying a bare pair of words.

`wifi <ssid> <password>` was once specified as the headless way to join a
network. It is `wifi join <ssid> [password]` now: `wifi` grew a status readout
and subcommands, and a bare pair of words that is sometimes an SSID and
sometimes a subcommand is a parser with a trap in it.
