# The console — every command, and what it does

Eighteen commands on UART0, through the CH343P bridge (the socket marked **UART**,
not the one marked **OTG** — §10.1). `COM6` on this machine.

Design documents describe *why*; this one describes what you can type.
`working-with-code.md` has how to open the port.

## Two rules that apply almost everywhere

**A command with no arguments reads; a command with arguments writes.** `wifi` is
a readout, `wifi join` is a change. There is no command that changes something as
a side effect of being asked a question.

**`config set` changes memory; `config save` changes the file.** Everything you
set survives until the next reboot and no further until you save it. The reply to
every `set` says so.

## What the device is

### `devstatus`
Every readout below, in one go, in the order in which one of them being wrong
stops the next from working: `status`, `buttons`, `led`, `request`, `key`,
`date`, `wifi`, `nats`, `keys`, `limits`. It exists so there is one thing to paste
into a bug report.

### `status`
```
firmware   approver-esp32-yubikey 4bbb764 (Aug 25 2026 21:23:35)
idf        v6.0.2 (built with v6.0.2)
chip       esp32s3 rev 0.2, 2 core(s)
mac        7c:e8:b1:b0:95:04
running    ota_0 at 0x020000, 2560 KB
uptime     0d 00h 20m 29s
heap       8511460 free, 8502848 lowest ever
storage    /spiffs, 3012 of 10474481 bytes used
```
The `running` line is how you find out whether the flash you just did landed.
`heap` is large because of the PSRAM (§10.13); the second number is the low-water
mark, which is the one that matters.

### `reboot`
Restarts now. No confirmation word — it undoes itself in seconds — but the line
it prints first says what it costs: anything `set` and not `save`d is gone.

## The two pieces of hardware

### `led`
What the device *is*, then what the light is doing about it. The two lines are
separate on purpose: a ranking that says `ready` next to an emitter showing red is
a bug you can only see if both are printed.
```
state      not-enrolled — no security key enrolled - run `key enrol`
showing    cyan, fast, 50%
rgb        0,255,255
ceilings   50% normal, 8% idle
wire       3671 writes, 0 failed
changes    4 state transitions since boot
stack      1636 B free
```
`changes` climbing while nobody is touching the device is a network that is
flapping, and it is the cheapest way to see that from a console.

### `led test`
Walks all fourteen states' colours, 1.5 s each, naming each one as it goes. It
goes through `SetFor`, so **the device is unaffected** — the ranking is untouched
and the next tick puts the real colour back. Safe with a request pending, which is
exactly when somebody wants to know what a colour looks like.

The two brightness ceilings are `config set led` and `config set ledidle`.
**Which colour means what is compiled in and is not a setting** (§10.17.2).

### `buttons`
```
BOOT   GPIO0   released raw released 679857 ms in this state
```
Both answers, because they disagree exactly when something is wrong: the raw level
is the wire, the other is what the debounce believes. A pin stuck low reads
pressed in both and held for the whole uptime, which is how a broken button tells
itself apart from an idle one.

### `buttons watch [seconds]`
Prints edges as they happen, with how long each press lasted. Default 10 s, max
120. **Not through a reset**: GPIO0 held across one is the ROM download strap and
nothing here would run.

## The approval loop

### `request`
What is pending, and the tally.
```
responder  answering approvals.* in the group 'approvers'
key        approver-esp32-yubikey
registered approver-esp32-yubikey
gate       a key is on the port
pending    Bash in console, 19 s left
off wire   0 received, 0 queued, 0 refused
gate said  1 asked, 0 approved, 0 button-denied, 0 nothing
on wire    0 replied (0 allow, 0 deny)
stack      responder 7996 B free, gate 7424 B free
```
The first four lines are the four things that have to be true before this device
is on the subject (§10.10 rule 5), spelled out rather than summarised — "not
subscribed" is one word for four different afternoons.

**`gate said … nothing`** is the count of decisions nobody made: timeouts,
unplugged keys, assertions that did not verify. None of them produced a reply, and
none of them is a `deny`.

A `lost` line appears only when it is not all zeros. Every number on it is a
decision a human made that nobody heard.

### `request test [seconds]`
### `request test <tool> <text …>`
Injects a synthetic request. It goes through **the whole path** — queued like a
real one, lights the LED like a real one, asks the key for a fingertip like a real
one — and is stopped one step from the wire: its reply subject is the sentinel the
responder refuses to publish into.

```
> request test 25
queued: Bash, 25 s
touch the key on the OTG port to allow, or tap BOOT to deny
nothing will be published — this request's reply subject is the test one
```

## The key (§10.18)

### `key`
What is on the port, what this device was enrolled against, and the counters.
```
plugged    nothing (or nothing with a FIDO interface)
enrolled   no — run 'key enrol' with a key plugged in
required   yes — no allow without a touch
gate       0 asked, 0 approved, 0 timed out
cable      0 attached, 0 detached, 0 claimed, 0 rejected
exchanges  0 total, 0 timed out, 0 framing, 0 key errors, 0 transfer
```
A `suspicious` line appears only when it is not zero — assertions that did not
verify, and credentials that were not the enrolled one. Neither is ever routine.

### `key info`
`authenticatorGetInfo`. No touch needed. Prints versions, options, and — the one
that matters — whether the key has a **PIN** set: this device cannot enter one
(§10.18), so a key that insists on one cannot be used here, and it is better to
find that out now than in the middle of an approval.

### `key enrol`
One `authenticatorMakeCredential`, one touch, and `fido.json` is written. This is
a **different step from `register`** and they do not replace each other (§10.18.1).
A failure changes nothing — the same ordering `registration.json` uses.

### `key test`
Asks for a real assertion over a **random** challenge, needs a touch, verifies the
answer against the enrolled public key, and **approves nothing**. It is the one
command that answers "would this device approve, if it were asked" without asking
it.

### `key forget now`
Deletes `fido.json`. The confirmation word is required for the same reason
`forget`'s is. **The credential stays on the key** — this firmware has no way to
remove one, and the reply says so rather than letting somebody believe a slot was
freed.

## This device's own key (§10.6)

### `keys`
The `key_id`, the public key, where the seed came from, and whether the boot
self-test passed. The public key is what appears in `handler-config.json` after a
registration.

### `keys selftest`
Re-runs the boot self-test against a vector generated by `lib/crypto.py`. It is
here because `CONFIG_LIBSODIUM_USE_MBEDTLS_SHA` is a seam that has historically
produced valid-looking but wrong signatures.

### `keys forget now`
Destroys the seed. The device gets a **new identity** on the next boot and every
registration against the old one is void.

### `register <token>`
The §6 exchange. Needs a bus connection. Prints the handler key it pinned, so an
operator can compare it once, by eye, with what the handler printed at startup.

### `forget now`
Deletes `registration.json`. The signing key is untouched.

## Settings

### `config`
The settings as the file has them, plus what a boot restore did if one happened.
Passwords are never printed (§10.15) — the shipped `wifi` lines say `password set`
or `not set`. The `approval` line shouts when the gate is open:
```
approval   key *** NOT REQUIRED ***, deny button on, 30 s to touch
```

### `config reload` / `config save` / `config restore`
Re-read the file discarding edits / write the current settings back / put
`config.init.json` over `config.json`. A reload and a restore both re-apply
everything through one hook, so a setting changed this way reaches the LED, the
radio, the clock and the bus without a reboot.

`restore` does **not** touch `registration.json` or `fido.json`.

### `config set <field> <value>`
`led`, `ledidle`, `requirekey`, `denybutton`, `touchtimeout`, `nats`, `tz`,
`sntp`, `sync`, `wifi`.

The two booleans are §10.18's gate. **`requirekey off` is the one setting that
makes this device less careful**, and it is the one that says so out loud — in the
reply, in a warning log line, and again at every boot.

`led` and `ledidle` are applied at once, because a brightness you cannot see the
effect of is one you cannot judge.

### `config zones [filter]`
The time zones known by name. Any POSIX rule is accepted too.

## Wi-Fi (§10.9)

### `wifi`
What the radio was asked to be, what it is doing, the network list with the
current one marked, the internet check, and the fallback access point.

### `wifi mode off|client|ap` · `wifi join <ssid> [password]` · `wifi forget <ssid>`
### `wifi static <n> <address> <netmask> <gateway> [dns1] [dns2]` · `wifi static <n> off`
### `wifi scan` · `wifi ping` · `wifi check [on|off|<address> …]` · `wifi retry`
As on the sibling board, unchanged. `wifi check` changes only the ping list and
does **not** touch the connection — a settings command that reconnects a working
link is a settings command people stop making.

## The bus (§10.5)

### `nats`
What it wants, what it is doing, the server, how many connects and drops, traffic,
and the live subscriptions.

### `nats connect` / `disconnect` / `retry` · `nats url <nats://host[:port]>`
### `nats sub <subject> [queue group]` · `nats unsub <subject>` · `nats pub <subject> [text …]`
The three at the end are a debugging surface. They cannot reach a verdict: nothing
they subscribe to feeds the responder, and nothing they publish is signed.

## The rest

### `limits`
The last `status` (§9.7) and `activity` (§9.10) documents off the bus, with how
long ago each arrived. A readout of a readout — nothing here can be acted on and
nothing here reaches a responder.

### `date` · `date sync` · `date set [utc] <YYYY-MM-DD> <HH:MM:SS>`
The system clock in UTC and through the configured zone. **A set survives until
the next reboot**: this board has no RTC (§10.13), and `date` says
`never set - no RTC on this board, and no sync yet` when nothing has told it.

### `ls` · `cat <path>`
The storage partition, and one file off it. `cat config.json` is how you read the
settings back without reflashing.

### `term` · `term smart` · `term dumb`
The up-arrow is off until you type `term`. **Never type `term` or `term smart` in a
scripted session**: in smart mode linenoise blocks on a cursor-position query
before each prompt, and a script that does not answer it leaves the console silent
until the board is reset. If it happens: send `\x1b[24;80R`, then `term dumb`.

## What is not here, and will not be

`poweroff` — nothing to switch off (§10.13). `screen`, `display`, `touch`,
`screenshot`, `clock` — no panel. `imu`, `power`, `audio`, `play` — no such chips.
`web` — no server, and §10.10 rule 4 is why there is unlikely to be one.
