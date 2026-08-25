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

Instead the *permission to sign* comes off a **security key on the OTG port**.
Before a single byte of a verdict is signed, the device asks a FIDO
authenticator for an assertion over *the exact bytes it is about to sign*, and
the key will not answer until somebody touches it. Three properties fall out of
that, and they are the whole reason this folder exists next to the other one:

* **a touch cannot be replayed onto a different request** — the challenge is the
  request's own session, nonce, tool, input hash and timestamp;
* **the device alone cannot approve anything** — compromised firmware with the
  Ed25519 seed still needs the physical key present and a finger on it;
* **the key alone cannot approve anything either** — it has never heard of NATS.

Both halves, or nothing. That is what a second factor is, and it is what the
single button on the sibling board does not have.

## The documents

These docs are section **10** of the project docs, the same section number the
sibling folder uses — because they describe **the same design on different
hardware**, and a comment that cites `§10.14.2` means the same rule in either
folder. Where this device genuinely differs, the section is new and its number is
new with it: **§10.17** is the light and **§10.18** is the key, and neither exists
next door.

Project-wide rules — TDD, the dependency allowlist — stay in
[`../CLAUDE.md`](../CLAUDE.md).

**This file carries what all the others assume, and nothing else**: what the
device is in the protocol (§10.2), the one decision that reaches outside this
folder (§10.3), and the rules that must not be softened (§10.10) — plus the
status summary below, whose long form is [`status.md`](status.md).

| File | What it owns |
|------|--------------|
| [`hardware.md`](hardware.md) | **§10.1** the board, the two USB-C sockets that are not interchangeable, and the one button that is also a boot strap; **§10.13** which parts have a job and which deliberately do not — including the clock this board does not have |
| [`led.md`](led.md) | **§10.17** the one output: the palette, the ranking behind it, and the UART trick that drives a WS2812 with no extra component. The whole user interface of this device is in this file |
| [`key.md`](key.md) | **§10.18** the security key: what it is and what it is not, enrolment as a separate step from registration, the four checks an assertion has to pass, and why there is no PIN |
| [`protocol.md`](protocol.md) | **§10.5** the NATS client and the subset of it this device uses; **§10.6** key custody, as designed and as shipped; **§10.7** registration, and the console it is driven from |
| [`firmware.md`](firmware.md) | **§10.9** Wi-Fi, the radio and the manager above it; **§10.14** the language, no heap, and the layer that comes first; **§10.15** where the configuration lives and the button that puts it back |
| [`tests.md`](tests.md) | **§10.11** the three tiers — host, parity vectors, device — what each pins, and what is still owed |
| [`build.md`](build.md) | **§10.4** the dependency set, including the one new component this board adds and the argument for it; **§10.12** the build, the target, the partition table and the size numbers |
| [`commands.md`](commands.md) | every console command the device answers and what each one does. Design documents describe why; that one describes what you can type |
| [`status.md`](status.md) | the row-by-row state of every piece — what runs on the board, what is written and untried, what is still a design. No decisions in it, and the fastest-moving file here |
| [`working-with-code.md`](working-with-code.md) | the mechanics: where ESP-IDF is installed on this machine, how to get a shell that has `idf.py`, which COM port is which, and how to drive the console from a script |

## Status: a working responder with the gate shut

**The device is real and the loop is closed up to the key.** It boots, mounts its
filesystem, derives an Ed25519 identity and self-tests it, joins Wi-Fi, connects
to the NATS server on the LAN, syncs its clock, watches §9.7's `status` and
§9.10's `activity`, registers with the handler over §6, and takes its place on
`approvals.*` in the `approvers` queue group. A request put on it appears as a
white flashing light, waits for a fingertip, expires with **no reply** if none
comes, and the counters say which of those happened. All of that has been done on
the board on this desk, against the real handler and the real NATS server.

**What has not been done on hardware is the key itself.** No FIDO authenticator
has been plugged into the OTG port yet, so §10.18's three layers — CTAPHID
framing, CBOR, CTAP2 — are **written, compiled, host-tested against hand-built
frames, and never once spoken to a real device.** [`status.md`](status.md) is row
by row about which is which, and it is the file to read before believing anything
in this paragraph is more finished than it says.

Because of that the device is, right now, in the state its own light calls
`not-enrolled` — cyan, flashing — and **it is deliberately not on `approvals.*`
while it is**: a device that cannot produce an `allow` must not take requests out
of a queue group away from responders that can (§6's "Multiple clients", and
`responder::Blocker::kNotEnrolled` is where that is enforced). That rule was
added after the first registration, when the device did exactly that.

Below the key, what runs: the WS2812 on GPIO48 driven off a UART (§10.17.1), a
fourteen-state ranking that decides what a single emitter says about a device
that is several things at once (§10.17), the BOOT button as a deny, the settings
file on SPIFFS with the restore that puts it back, the Wi-Fi radio with a manager
above it, the clock's SNTP half, the bus, and a console on the CH343P bridge with
a command per piece of it ([`commands.md`](commands.md) is the list).

Read §10.3 before anything else: it is the one part of this that changes
something outside this folder.

## 10. `approver-esp32-yubikey/` — the firmware

### 10.2 What the device is in the protocol

A responder, indistinguishable from the software one to everything that verifies
it:

| | Value |
|---|---|
| `key_id` | `approver-esp32-yubikey` (its own — **not** shared with `approver-esp32`) |
| `key_type` | `ed25519` (§10.6 explains why, and what it costs) |
| Subscribes | `approvals.*`, queue group `approvers` |
| Registers over | `registrations`, §6, on the device (§10.7) |
| `reason` | always `""` — no keyboard, same call `responder_yubikey.py` makes (§8.7) |
| `updated_input` | **never sent** |

**The verdict's signature is the device's Ed25519 key, not the security key's**,
and that is worth stating here rather than only in §10.18 because it is the thing
somebody will assume otherwise. Swapping the protocol's signature to something a
FIDO key emits would have meant a second scheme in `crypto.py`, a second
registration path, and an ARKG flow (§8) on a microcontroller — for a property
this design already has by other means. What the security key produces is
**permission to use** the device's key, bound to one request.

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
   as separate numbers for exactly this reason.

3. **Never publish into a dead inbox.** The connection generation is recorded when
   a decision is made and checked before the reply goes out. A reply into an inbox
   that no longer exists is worse than silence, because it looks like an answer.

4. **Nothing that can reach a verdict may be reachable from the network.** There is
   no web server on this device and no console over the bus. The two ways to
   produce an `allow` are a fingertip on a security key and — only with
   `approval.requireKey` explicitly off — a two-second hold on the button.

5. **The device does not subscribe unless it could actually answer.** §6's queue
   group means each request reaches exactly *one* responder, so a device that
   subscribed while unable to sign would be taking requests away from responders
   that can and answering them with silence. Key, registration, **enrolment**, and
   a connection — or it is not on the subject at all.

6. **The key's answer is never inferred, and neither is its absence.** An assertion
   that does not verify is not a `deny` and is not an `allow`: it is a
   `bad-signature`, the loudest outcome this firmware has, and it produces no
   reply. §10.18 has the four checks and why each one is load-bearing.

A change that softens any of these is a change to this file first.
