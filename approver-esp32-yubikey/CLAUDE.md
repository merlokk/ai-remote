# approver-esp32-yubikey — the responder as a device with a key in it (ESP32-S3, ESP-IDF)

A fifth responder for the approval flow in
[`../approver/CLAUDE.md`](../approver/CLAUDE.md) §6/§7, alongside
`approver/responder.py` (software key), `approver/responder_yubikey.py` (key on a
YubiKey), [`approver-web/`](../approver-web/CLAUDE.md) (the page) and
[`approver-esp32/`](../approver-esp32/CLAUDE.md) (the board with a screen). Same
subjects, same `handler-config.json` allowlist, same signing bytes — `hook.py`,
`protocol.py` and `registration_handler.py` must not change for it, and if they
do, the design is wrong.

**The difference from the board with a screen is not the screen.** That device
answers a request with one press on a case: the operator is the second factor,
and what proves they were there is that a finger touched a piece of glass. This
one has no glass, and it does not try to replace it with a button — a button is a
thing anybody in the room can press, and firmware that had been tampered with
could press it in software.

Instead **the signature itself is made inside a security key on the OTG port**.
The device derives an ARKG public key from the authenticator's seed key (§8's
`previewSign` flow, the same one `approver/responder_yubikey.py` uses), registers
*that* through §6, and from then on every verdict is an ECDSA P-256 signature the
key produces — and it will not produce one until somebody touches it. Four
properties fall out, and they are the whole reason this folder exists next to the
other one:

* **a touch cannot be replayed onto a different request** — what is signed is the
  request's own session, nonce, tool, input hash, verdict and timestamp;
* **the device alone cannot approve anything, literally** — there is no signing
  key in this board's flash to steal. The private half is reconstructed inside the
  authenticator from a key handle and never leaves it;
* **the key alone cannot approve anything either** — it has never heard of NATS,
  and the derived key means nothing without the `ikm` this device holds;
* **a `deny` costs a touch too** — the device cannot put its name to one either.
  That is the one place this design is more awkward than the button next door, and
  §10.18.5 says so rather than hiding it.

Both halves, or nothing. That is what a second factor is, and it is what the
single button on the sibling board does not have.

**Nothing on the host side knows any of this.** `hook.py`, `protocol.py` and
`registration_handler.py` are unchanged: a `key_type: "p256"` responder is one
`lib/crypto.py` has verified since §8.7, and this device is indistinguishable from
`responder_yubikey.py` to everything that checks it.

**And nothing here shows anything, on the device.** There is no display of any
kind, so there is nothing to render a request, a session's spending or a session's
activity on, and this device subscribes to no subject but `approvals.*`. It has one
output, and its whole vocabulary is §10.17's sixteen states.

What it does have is a **page**, on a phone: §10.16's configuration site, which is
the sibling board's server on hardware that needs it more — with no glass and no
keyboard, the alternative to it was a cable. It reports state and writes two
sections of `config.json`, and the one thing it will never do is ask the key for a
touch (§10.10 rule 4).

**There is also no clock**, for the same reason: no RTC — the board has no I²C bus
for one (§10.13) — and no SNTP, because a time that is only right once a server has
been asked, on a device with nowhere to show it, is machinery with no consumer.
§7's `ts` is echoed from the request and never re-derived, and every interval this
firmware measures is monotonic.

## The documents

These docs are section **10** of the project docs, the same section number the
sibling folder uses — because they describe **the same design on different
hardware**, and a comment that cites `§10.14.2` means the same rule in either
folder. Where this device genuinely differs, the section is new and its number is
new with it: **§10.17** is the light and **§10.18** is the key, and neither exists
next door.

**§10.16 goes the other way and is worth naming for it**: the configuration site
keeps its number because it is the same component, ported rather than rewritten —
the same whitelist, the same gate, the same desired-state reconciler, and the two
crashes and one double free that shaped them did not have to be re-learned here.
[`web.md`](web.md) lists the six things that do differ and then stops.

Project-wide rules — TDD, the dependency allowlist — stay in
[`../CLAUDE.md`](../CLAUDE.md).

**This file carries what all the others assume, and nothing else**: what the
device is in the protocol (§10.2), the one decision that reaches outside this
folder (§10.3), and the rules that must not be softened (§10.10) — plus the
status summary below, whose long form is [`status.md`](status.md).

| File | What it owns |
|------|--------------|
| [`hardware.md`](hardware.md) | **§10.1** the board, the two USB-C sockets that are not interchangeable, and the one button that is also a boot strap; **§10.13** what is not on this board at all — no I²C bus and so no clock, no PMIC, no accelerometer and no codec; no display and no touch — and what each absence costs |
| [`led.md`](led.md) | **§10.17** the one output: the palette, the ranking behind it, and the UART trick that drives a WS2812 with no extra component. The whole user interface of this device is in this file |
| [`key.md`](key.md) | **§10.18** the security key: that it is the *signer*, the ARKG derivation that gives it a key to sign with, enrolment and what it costs the registration, the five checks an answer has to pass, why a deny needs a touch, and why there is no PIN |
| [`protocol.md`](protocol.md) | **§10.5** the NATS client and the subset of it this device uses; **§10.6** key custody, as designed and as shipped; **§10.7** registration, and the console it is driven from |
| [`firmware.md`](firmware.md) | **§10.9** Wi-Fi, the radio and the manager above it; **§10.14** the language, no heap, and the layer that comes first; **§10.15** where the configuration lives and the button that puts it back |
| [`web.md`](web.md) | **§10.16** the configuration site: why on this board it is the only way in that needs no cable, the six things that differ from the board next door, the whitelist that keeps an ARKG enrolment off a LAN, the two sections of the settings file a network may not touch, the password on the door, and the numbers |
| [`tests.md`](tests.md) | **§10.11** the three tiers — host, parity vectors, device — what each pins, and what is still owed |
| [`build.md`](build.md) | **§10.4** the dependency set, including the one new component this board adds and the argument for it; **§10.12** the build, the target, the partition table and the size numbers |
| [`commands.md`](commands.md) | every console command the device answers and what each one does. Design documents describe why; that one describes what you can type |
| [`status.md`](status.md) | the row-by-row state of every piece — what runs on the board, what is written and untried, what is still a design. No decisions in it, and the fastest-moving file here |
| [`working-with-code.md`](working-with-code.md) | the mechanics: where ESP-IDF is installed on this machine, how to get a shell that has `idf.py`, which COM port is which, and how to drive the console from a script |
| [`user-manual.md`](user-manual.md) | the only document here written for whoever is **holding** the device rather than changing it: the two sockets, the one light, the gesture that approves and the two that refuse, first-time setup, and a troubleshooting table keyed on what the light is doing. Three photographs of the real board in [`images/`](images/) carry the states words are worst at — green at rest, white with the key asking, red with the deny waiting to be signed. **It decides nothing** |
| [`README.md`](README.md) | the short form, for somebody arriving rather than working here: what the device is, the four hardware facts, build and flash, first-time setup and what the light means. **It owns no section and decides nothing** — every line in it is a summary of one of the files above, which stay authoritative |

## Status: a working responder with no signing key yet

**The device is real and the loop is closed up to the key.** It boots, mounts its
filesystem, joins Wi-Fi, connects to the NATS server on the LAN, and — in the
shape it had before §10.18 changed the signer — registered with the handler over
§6 and took its place on `approvals.*` in the `approvers` queue group. A request
put on it appears as a white flashing light, waits for a fingertip, expires with
**no reply** if none comes, and the counters say which of those happened. All of
that has been done on the board on this desk, against the real handler and the
real NATS server.

**The key has met hardware, and this device now holds a signing key derived from
a real one.** A YubiKey 5 on the OTG port was enumerated, interrogated and
enrolled: §10.18's four layers — CTAPHID framing, CBOR, CTAP2 with `previewSign`,
and the ARKG derivation — all ran, the derivation's two curve operations agree with
Python **on the chip** (`key selftest`, 661–670 ms), and `fido.json` holds a key
this device signs as. [`status.md`](status.md) is row by row and it is the file to
read before believing anything here is more finished than it says.

**And the key has signed.** `key test` was run twice against it: an assertion came
back, §10.18.3's five checks all passed, and the fifth — the verdict signature
against the **derived** public key — is the equality this whole design rests on.
Nothing on the host side could have checked it: it says the key this chip derived
is one the authenticator can reconstruct the private half of.

**And the loop is closed.** A real request has gone
`request → white light → touch → signed reply → hook`, and `hook.verify_reply` —
the verifier Claude Code itself would use — called the reply **`TRUSTED`**. This
device is a working responder, indistinguishable from the other four to everything
that checks one, and the signature it answered with was made inside a security key
while somebody was touching it.

The only step left is cosmetic by comparison: the request arriving from a live
Claude Code session's `PermissionRequest` rather than from the probe that sends the
same bytes.

Because of that the device is, right now, in the state its own light calls
`not-registered` — magenta — and **it is deliberately not on `approvals.*` while
it is**: it must not take requests out of a queue group away from responders that
can answer them (§6's "Multiple clients", and `responder::Blocker` is where that is
enforced). That rule was added after the first registration, when the device did
exactly that. The rung below it, `not-enrolled`, is where a device with no key at
all sits — **and it is the lower of the two on purpose**, because `register`
refuses without an enrolment (§10.18.1), so telling an operator to mint a one-time
token first would cost them the token.

**And there is no registration at all on the device now.** The one made before
§10.18 named an Ed25519 key this firmware no longer signs with, and
`registration.json` is no longer there either — the device says so at boot and the
console says `registered no`. The enrolment is done, so what is left is one fresh
token: `register <token>`. The `STALE` path — a registration naming a key that is
no longer the enrolled one — is still real and still enforced at every boot
(§10.18.1); it just is not what this device is showing.

Below the key, what runs: the WS2812 on GPIO48 driven off a UART (§10.17.3), a
sixteen-state ranking that decides what a single emitter says about a device
that is several things at once (§10.17), the BOOT button as a deny, the settings
file on SPIFFS with the restore that puts it back, the Wi-Fi radio with a manager
above it, the bus, and a console on the CH343P bridge with a command per piece of
it ([`commands.md`](commands.md) is the list).

**And one thing that is new: §10.16's configuration site.** The sibling board's
`esp_http_server`, its whitelist, its basic-auth gate and its write path, ported —
plus the seven pages and the `app.js` they share. On this device it is the only way
in that needs no cable, which is why it was worth having: there is no glass here to
type an SSID on. It costs 56,576 bytes of the image (49 % of the slot still free)
and 11,064 bytes of heap while it is up.

**It runs, on the board, and it found a bug on the way.** All three API endpoints,
the whitelist refusing `fido.json` as an indistinguishable 404, the write path
refusing the gate's own settings *by name*, both halves of the reboot guard, and
eight ways of getting the credential wrong — all over the LAN. `web cycle 20` then
answered §10.16's founding question on this chip for the first time: **+0 bytes over
twenty rounds**, so there is no leak on an S3 either. The bug was the one constant
the port could not inherit — a 4 KB task stack that `/api/devstatus` left 116 bytes
of, because *this* board's dump includes the key readouts — and it rebooted the
device several requests after the dump that caused it. [`status.md`](status.md) has
why that was hard to see and [`web.md`](web.md) has the numbers.

**What has not happened is an actual page.** The site lives in the SPIFFS image, so
serving HTML needs a full `idf.py flash`, and on this board that erases `fido.json`
and costs a `key enrol` with the key in hand (§10.18.1). Everything behind the pages
is verified; the markup is checked from the host by
`tests/test_esp32_web_pages.py`, which now reads both boards' sites from one copy of
the rules.

And one thing that is **no longer there at all**: `components/crypto` used to derive
an Ed25519 identity at boot, which since §10.18 signed nothing. It has been deleted
— the seed, the eFuse route, `Sign`, and the seed's 32 bytes in unencrypted NVS,
which the firmware now **erases** on any board that has them. What is left of
libsodium verifies the *handler's* reply (§6's server key is Ed25519 by fixed
protocol) and provides base64, and neither ever needed a key of ours. **There is no
private key on this board of any kind** (§10.6), which is the sentence the whole
design was for.

Read §10.3 before anything else: it is the one part of this that changes
something outside this folder.

## 10. `approver-esp32-yubikey/` — the firmware

### 10.2 What the device is in the protocol

A responder, indistinguishable from the software one to everything that verifies
it:

| | Value |
|---|---|
| `key_id` | `approver-esp32-yubikey` (its own — **not** shared with `approver-esp32`) |
| `key_type` | `p256` — an ARKG-derived key, signed inside the security key (§10.18) |
| Subscribes | `approvals.*`, queue group `approvers` |
| Registers over | `registrations`, §6, on the device (§10.7) |
| `reason` | always `""` — no keyboard, same call `responder_yubikey.py` makes (§8.7) |
| `updated_input` | **never sent** |

**The verdict's signature is the security key's, and the key `key_id` names is
derived from it** (§10.18). What §6 registers is the compressed P-256 point of an
ARKG-derived key; what §7 carries is an ECDSA signature the authenticator made
while somebody was touching it. `lib/crypto.py` has verified that pair since §8.7,
so the handler and the hook needed no change — which is the test this design had
to pass, and root §2's rule that the five responders look the same from outside.

Two consequences worth having here rather than only in §10.18: **the enrolment is
the identity**, so a re-enrolment invalidates the registration and needs a fresh
token; and **there is no key on this board to steal**, because the private half
lives inside the authenticator.

`key_id` is a **constant in `protocol/registration.h`**, not a setting. It is
half of what an allowlist entry is bound to, and an operator who could change it
could make this device answer as another one.

### 10.3 What this needs on the LAN

The one decision here that reaches outside this folder: **the NATS server has to
be reachable from the network this board is on**, by IP, without TLS and without
credentials. That is the same requirement [`approver-esp32/`](../approver-esp32/CLAUDE.md)
§10.3 states and the same server — `nats/docker-compose.yml` — and this device
adds nothing to it.

Two consequences, both inherited:

* **the trust boundary is the router.** Anything on that network can publish on
  `approvals.*`. §10.10 is what makes that survivable: a request this device
  cannot make sense of is dropped, and the only thing that turns a request into
  an `allow` is a human touching a key;
* **the 64 KB payload ceiling** in `nats/nats-server.conf` is a fail-safe for
  this device as much as for the other one. `ui::Request` bounds every field it
  keeps, and a payload past those bounds is refused rather than truncated.

### 10.10 The rules that may not be softened

Everything in this folder is negotiable except these. They are the same five the
sibling board keeps, plus one that is this device's own.

1. **No reply is the safe outcome.** Every failure — a payload that will not
   parse, a queue that is full, a key that is not there, a touch that never came,
   a signature that fails, a socket that dropped — ends in *nothing* being
   published, the hook timing out, and Claude Code asking in its own terminal.
   There is no path that invents a verdict.

2. **A timeout is not a deny.** They are different outcomes and they are counted
   apart. A deny is a statement somebody made; silence is what a device says when
   nobody made one. `responder::Status` has `gate_declined` and `button_denied`
   as separate numbers for exactly this reason — and since §10.18 a deny has to be
   *signed* by the key, so the tap that chooses one and the touch that makes it
   real are two events and only the second produces a reply.

3. **Never publish into a dead inbox.** The connection generation is recorded when
   a decision is made and checked before the reply goes out. A reply into an inbox
   that no longer exists is worse than silence, because it looks like an answer.

4. **Nothing that can reach a verdict may be reachable from the network.** Since
   §10.18 there is exactly **one** way to produce an `allow`: a fingertip on a
   security key. `approval.requireKey` — the setting that used to open a second
   way — was deleted with the signing key it belonged to (§10.18.6), because a
   device that holds no private key cannot be told to approve without one.

   **This rule used to be enforced by there being no server at all, and it is not
   any more**: §10.16's configuration site is on this device, and it is the only
   way in that needs no cable. What holds the rule up now is narrower and written
   down in three places rather than assumed:

   * the site's write path is a **whitelist of two sections**, `wifi` and `nats`,
     and `approval` — the timeout a request waits for a touch, and whether BOOT
     may deny — is refused *by name*. So is `led`, so a network cannot turn the
     light off on a device whose only user interface is that light. Two host tests
     are that rule rather than this paragraph;
   * the site can never ask the key for anything. `components/web` does not
     require `responder` or `fido` and cannot reach the gate; the counters, the
     key's two booleans and the light's word arrive as a struct `main` fills;
   * and there is still **no console over the bus**, which is the half of this rule
     that has not changed.

   The one thing the site *can* do to this device is restart it, behind a
   confirmation word — which takes nothing away that does not come back by itself,
   and cannot produce a verdict.

5. **The device does not subscribe unless it could actually answer.** §6's queue
   group means each request reaches exactly *one* responder, so a device that
   subscribed while unable to sign would be taking requests away from responders
   that can and answering them with silence. Enrolment, a registration **that names
   the key currently enrolled**, and a connection — or it is not on the subject at
   all.

   The middle clause is §10.18.1's: the registered key comes from the enrolment, so
   a `key enrol` makes the handler's allowlist entry worthless. `registration.json`
   records which key it was made for and the two are compared at every boot, because
   a device answering with signatures the hook rejects is worse than one not
   answering — it is invisible.

6. **The key's answer is never inferred, and neither is its absence.** An answer
   that does not verify is not a `deny` and is not an `allow`: it is a
   `bad-signature`, the loudest outcome this firmware has, and it produces no
   reply. §10.18.3 has the five checks and why each one is load-bearing — including
   the last, where the device verifies the key's signature against its own
   registered public key **before** publishing it, so a mismatch is one log line
   rather than an approval that silently never lands.

A change that softens any of these is a change to this file first.
