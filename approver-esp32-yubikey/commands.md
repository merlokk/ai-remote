# The console — every command, and what it does

Sixteen commands on UART0, through the CH343P bridge (the socket marked **UART**,
not the one marked **OTG** — §10.1). `COM6` on this machine.

One of them lost a subcommand rather than gaining one: `keys forget now` went with
the Ed25519 identity it deleted (§10.6).

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
`wifi`, `nats`, `keys`. It exists so there is one thing to paste into a bug
report.

### `status`
```
firmware   approver-esp32-yubikey c7036c2 (Aug 26 2026 18:15:53)
idf        v6.0.2 (built with v6.0.2)
chip       esp32s3 rev 0.2, 2 core(s), wifi + ble
flash      16 MB, build assumes 16MB
psram      8 MB, octal at 80 MHz, 8380928 free
mac        7c:e8:b1:b0:95:04
running    ota_0 at 0x020000, 2560 KB
uptime     0d 00h 20m 29s
heap       157119 internal free, 148347 lowest ever
storage    /spiffs, 4267 of 10474481 bytes used
```

The `running` line is how you find out whether the flash you just did landed.

**`flash` and `psram` report what the chip came up with, next to what the build
assumed.** `hardware.md` §10.1 says both are assumptions — this board ships in
several memory variants, and a wrong PSRAM mode is a device that fails somewhere
far from the mistake — so the two halves of each line are there to be compared. A
`psram` line reading `none` on a build that expects some is the loudest thing
`status` can say.

Neither line says *embedded* or *external*, because on this target nothing knows:
the esp32s3's `chip_info.c` reports `wifi + ble` and nothing else, so those bits are
always zero and a word derived from them would be a constant pretending to be a
measurement.

**`heap` is internal RAM, and only internal RAM.** That is the number worth
watching: task stacks, every static buffer and anything a driver wants during an
interrupt come out of it, a stack may not live in PSRAM at all (§10.13), and it is
what decided `responder::kGateStackBytes`. The PSRAM's own free space is on the
`psram` line, where it cannot be mistaken for headroom that a stack could use. The
second number is the low-water mark, which is the one that matters.

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
showing    cyan, fast, 15%
rgb        0,255,255
ceilings   15% normal, 7% idle
wire       3671 writes, 0 failed
changes    4 state transitions since boot
stack      1636 B free
```
`changes` climbing while nobody is touching the device is a network that is
flapping, and it is the cheapest way to see that from a console.

### `led test`
Walks all fifteen states' colours, 1.5 s each, naming each one as it goes. It
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
key        AmNqT1p2...  (the enrolled key this device signs as)
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

`registered` has a third answer besides a name and `no`: **`STALE - for <key>, not
the key enrolled now`**, which is what a `key enrol` leaves behind (§10.18.1). The
device stays off the subject until a fresh token puts the current key in the
handler's allowlist.

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
touch the key on the OTG port to allow, or tap BOOT to deny and touch it to sign that
nothing will be published — this request's reply subject is the test one
```

**A deny needs the key too** (§10.18.5): the button chooses the verdict, the key
signs it. Tapping BOOT and then walking away produces no reply at all, which is the
safe outcome and is counted as `nothing` rather than as a deny.

## The key (§10.18)

### `key`
What is on the port, what this device was enrolled against, and the counters.
```
plugged    nothing (or nothing with a FIDO interface)
enrolled   no — run 'key enrol' with a key plugged in
gate       0 asked, 0 approved, 0 timed out
cable      0 attached, 0 detached, 0 claimed, 0 rejected
exchanges  0 total, 0 timed out, 0 framing, 0 key errors, 0 transfer
```
With an enrolment there is one more line, and it is the important one:
```
signs as   AmNqT1p2…  (p256)
```
**That string is what the handler's allowlist has to contain** (§10.2). Comparing
it by eye with `handler-config.json` is how a stale registration gets found in
seconds instead of by watching approvals silently fail.

A `suspicious` line appears only when it is not zero — assertions that did not
verify, and credentials that were not the enrolled one. Neither is ever routine.

### `key info`
`authenticatorGetInfo`. No touch needed.
```
versions   FIDO_2_1 U2F_V2
options    rk yes, up yes, uv no
previewSign yes — this key can be enrolled
pin        not set
aaguid     f4ce5fc057d346f5a736efb7d5bc63b5
maxmsg     1536 bytes
```
Two of those lines decide whether the key can be used here at all, and they are
different kinds of no:

* **`previewSign`** is the harder one. The signing key is *derived* from that
  extension (§10.18), so a key that does not advertise it has nothing to derive
  from — and no firmware update on this side can change that. `key enrol` refuses
  such a key rather than spending a touch on a `makeCredential` whose failure
  arrives as a CTAP status naming no cause;
* **`pin`** is the softer one: this device has no way to enter one, so a key that
  insists on it cannot be used here — but a PIN can be removed.

### `key enrol`
One `authenticatorMakeCredential` carrying `previewSign.generateKey`, one touch,
and `fido.json` is written: a credential, an ARKG seed key, and the key this device
derives from it. **This is where the signing key comes from** (§10.18), so it comes
*before* `register` — which refuses with a sentence saying so if nothing is
enrolled.

**A key that does not advertise `previewSign` is refused before the touch**, using
the `getInfo` that costs nothing:
```
> key enrol
this key does not advertise previewSign, so it cannot be enrolled here.
the signing key is derived from that extension (§10.18) — without it
there is nothing to derive from. `key info` lists what this key does.
```

And when the touch simply never comes, the key refuses the request itself:
```
> key enrol
touch the key…
not enrolled: the key said no — operation denied (CTAP 27)
           the usual cause is that nobody touched it in time
```
That is the failure worth recognising on sight, because the spec's wording for it
names no cause and reads like a broken device. It is a **nothing**, not a deny
(§10.10 rule 2).

A failure changes nothing — the same ordering `registration.json` uses. A *success*
changes a great deal, and the reply says so:
```
enrolled on 1050:0407, aaguid d7781e5d…, credential 64 bytes
written to fido.json
signs as   AmNqT1p2…  (p256)
this is a NEW key — the registration is stale. run 'register <token>'
with a fresh token from the handler before this device can answer.
```
The key is new, so the allowlist entry naming the old one is worthless (§10.18.1).

### `key test`
Asks the key to sign a **random** challenge — the whole approval path except the
request. It needs a touch, it runs all five checks (§10.18.3), and it **approves
nothing**: the bytes it signed belong to no request, so they are not a decision
about anything. It is the one command that answers "would this device approve, if it
were asked" without asking it.

```
> key test
touch the key… (the light is blue and fast while it waits)
approved — the key confirmed this request
signed and verified in 7232 ms, 71 bytes: MEUCIQCJnLsn…
nothing was approved: this challenge belongs to no request, so the
bytes it signed are not a decision about anything (§10.18).
```

**The light is blue and fast while it waits**, and goes back the moment the key
answers — the same for `key enrol` (§10.17). The time is the human's, not the
device's: a signature takes tens of milliseconds and the rest is somebody walking
over to the key.

### `key selftest`
**Needs no key on the port**, and it is the one part of §10.18 that can be checked
on a bare board. It runs the ARKG derivation's two curve operations — the ECDH and
the point addition — on this chip, against a vector generated by Python and
compiled into the firmware (§10.18.2), and compares the derived public key and key
handle.

```
> key selftest
the curve agrees with Python: A6AM+XYnrD8w… (670 ms)
```

A failure means no security key would ever have worked here: the device would
register a public key whose private half no authenticator can reconstruct, every
reply would be rejected by the hook, and from the desk that looks exactly like a
device that is not answering. **Do not register a board that fails this.**

### `key forget now`
Deletes `fido.json`. The confirmation word is required for the same reason
`forget`'s is. **The credential stays on the key** — this firmware has no way to
remove one, and the reply says so rather than letting somebody believe a slot was
freed.

## What this device is to the handler (§10.6)

**There is no key of this device's own any more**, and `keys` is where that is said
out loud. §10.18 moved the signer into the security key — `key` above prints what it
signs as — and §10.6's Ed25519 identity has been **deleted**, along with the seed it
kept in NVS. What is left of libsodium here verifies the *handler's* reply (§6's
server key is Ed25519 by fixed protocol) and provides base64.

### `keys`
```
key id     approver-esp32-yubikey
signs as   see 'key' - an ECDSA P-256 key derived from the security key (§10.18)
private    none on this device
verifier   ready - libsodium verifies against its own vector
registered yes, as approver-esp32-yubikey
handler key Q15MkgcK2zYbfbLfxF2CFN8jyRRluEjWxbHnhRL1Zv0=
registered 2026-08-26 14:43:00 UTC
```
The `key_id` the handler knows this board by, where the signature actually comes
from, and whether the handler's replies can still be checked. The `private` line is
there to be read rather than inferred: it is the one fact §10.6 exists to state.

Once registered it also prints the pinned handler key and when the registration
happened — **in UTC, and it says so**, because that instant comes off the
handler's clock in the reply and there is no clock on this board to render it
through (§10.13).

### `keys selftest`
Two checks against a vector generated by `lib/crypto.py`: a real signature must
verify, **and a one-bit-flipped copy of it must not**. The second is the one people
leave out, and without it a verifier that says yes to everything passes.

It is here because `CONFIG_LIBSODIUM_USE_MBEDTLS_SHA` is a seam that has
historically produced valid-looking but wrong Ed25519 output, and a broken SHA
breaks checking a signature as thoroughly as making one. It signs nothing.

A failure is not cosmetic: `crypto::Ready()` goes false and the device stays off
`approvals.*` (`request` reports it), because a board that cannot check §6's reply
cannot know whose key it pinned.

### ~~`keys forget now`~~ — gone
It deleted the stored seed, and there is no seed. Typing `keys forget` answers with
why rather than "unknown command", and names the one that does what the operator
probably meant: **`key forget now`**, the enrolment, which is what a registration is
actually bound to (§10.18.1).

### `register <token>`
The §6 exchange. Needs a bus connection **and an enrolment** — the key it registers
is the enrolled one (§10.18.1), and with nothing enrolled it refuses rather than
registering something weaker. Prints the handler key it pinned, so an operator can
compare it once, by eye, with what the handler printed at startup.

`request` reports a registration made for a *different* key as `STALE`, which is
what a `key enrol` leaves behind.

### `forget now`
Deletes `registration.json`. The signing key is untouched.

## Settings

### `config`
The settings as the file has them, plus what a boot restore did if one happened.
Passwords are never printed (§10.15) — the shipped `wifi` lines say `password set`
or `not set`.
```
approval   deny button on, 30 s to touch
```
There is no line about whether the key is required, because there is no answer but
yes (§10.18.6).

### `config reload` / `config save` / `config restore`
Re-read the file discarding edits / write the current settings back / put
`config.init.json` over `config.json`. A reload and a restore both re-apply
everything through one hook, so a setting changed this way reaches the LED, the
radio and the bus without a reboot.

`restore` does **not** touch `registration.json` or `fido.json`.

### `config set <field> <value>`
`led`, `ledidle`, `denybutton`, `touchtimeout`, `nats`, `wifi`.

**`requirekey` is gone**, and typing it answers with why rather than "unknown
setting": it switched the security key off, back when this device signed with a key
of its own, and there is no such key any more (§10.18.6). An operator typing it is
holding an old instruction, not making a typo.

`led` and `ledidle` are applied at once, because a brightness you cannot see the
effect of is one you cannot judge.

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

### `ls` · `cat <path>`
The storage partition, and one file off it. `cat config.json` is how you read the
settings back without reflashing.

### `term` · `term smart` · `term dumb`
The up-arrow is off until you type `term`. **Never type `term` or `term smart` in a
scripted session**: in smart mode linenoise blocks on a cursor-position query
before each prompt, and a script that does not answer it leaves the console silent
until the board is reset. If it happens: send `\x1b[24;80R`, then `term dumb`.

## What there is no command for

Sixteen is the whole list, and what is missing from it is missing because the thing
it would control is not on this board (§10.13) — not switched off, not postponed:

| No command for | Why |
|----------------|-----|
| a display or a touch surface | there is neither. One WS2812 is the whole interface (§10.17) and `led` is its command |
| an accelerometer | not on this board, and nothing here would read one |
| sound | no codec and no speaker. The light is the whole notification |
| power | no PMIC and no battery: this board is mains-powered over USB, and there is nothing to switch off |
| a clock or a date | **no clock of any kind.** No RTC and no SNTP: §7's `ts` is echoed from the request and never re-derived, nothing else here reads a wall-clock time, and there is nowhere to show one |
| a web server | there is none, and §10.10 rule 4 is why there is unlikely to be one |
