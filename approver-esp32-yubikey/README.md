# approver-esp32-yubikey

A Claude Code permission responder as firmware, on a **YD-ESP32-S3** with a
**FIDO security key on its USB OTG port**.

Claude Code asks for permission, the `PermissionRequest` hook puts the request on
NATS, this device shows it as a flashing light, and the answer is signed **inside
the security key** while somebody is touching it. The hook verifies that signature
and hands Claude Code `allow` or `deny`.

There is **no signing key on this board**. It derives an ARKG public key from the
authenticator's seed key, registers *that*, and every verdict is an ECDSA P-256
signature the key produces. The device cannot approve alone, the key cannot
approve alone, and a touch cannot be replayed onto a different request.

The protocol, the registration flow and the four other responders are the
repository's: [`../CLAUDE.md`](../CLAUDE.md), [`../approver/`](../approver/CLAUDE.md).

## What it is on the wire

| | |
|---|---|
| `key_id` | `approver-esp32-yubikey` (a constant, not a setting) |
| `key_type` | `p256` — the ARKG-derived key, signed inside the authenticator |
| Subscribes | `approvals.*`, queue group `approvers` |
| Registers over | `registrations`, with a one-time token typed on the console |

Nothing on the host side knows this device exists: `hook.py`, `protocol.py` and
`registration_handler.py` are unchanged.

## The hardware, in four lines

* **two USB-C sockets, not interchangeable** — `UART` (CH343P bridge) is flashing,
  the log and the console; `OTG` is a USB **host** for the security key;
* **one WS2812 on GPIO48** is the whole user interface — no screen, no sound;
* **one button** (`BOOT`, GPIO0) chooses `deny`, and holds a config restore;
* no I²C bus, and so no clock: `ts` is echoed from the request, never re-derived.

## Build, flash, talk to it

ESP-IDF **v6.0.2**, target `esp32s3`. [`working-with-code.md`](working-with-code.md)
has this machine's paths and the one line that gets a shell with `idf.py` in it.

```powershell
idf.py set-target esp32s3
idf.py build
idf.py -p COM6 app-flash        # prefer this - a full `flash` erases the settings,
idf.py -p COM6 monitor          # the registration and the key enrolment
```

Host tests need no board and no key:

```
host_test\run.cmd               # 358 tests
```

## First-time setup, on the console

```
wifi join <ssid> <password>
config set nats nats://<host>:4222
config save
key selftest                    # the ARKG derivation on this chip - needs no key
key enrol                       # one touch. prints the p256 key it will sign as
register approver-esp32-yubikey.<one-time token from the handler>
```

Enrolment comes first: the key being registered *is* the enrolment's, so a
re-enrolment invalidates the registration and needs a fresh token.
[`commands.md`](commands.md) is every command the device answers.

## The light

Fifteen states, ranked — one emitter can only say one thing at a time. The ones
worth memorising:

| | |
|---|---|
| **white, fast** | a request is waiting for you — touch the key |
| **green, breathing** | connected, registered, enrolled, nothing to do |
| yellow, fast | no storage, no key, no Wi-Fi, no internet or no bus — plug it into a network that has the bus |
| cyan, fast | nothing enrolled — `key enrol`. Shown **before** the one below, because enrolment comes first |
| magenta | enrolled, not registered — `register <token>` |
| red | solid at boot; blinking is a fault |

[`led.md`](led.md) is the full table and the reasoning under it.

## Status

**The loop runs up to the signature.** Boot, filesystem, Wi-Fi, NATS, the request
queue and every failure path have been done on the board on this desk — and so has
the enrolment: a YubiKey 5 on the OTG port was enumerated, interrogated and
enrolled, so all four layers under §10.18 (CTAPHID, CBOR, CTAP2 with `previewSign`,
and the ARKG derivation) have run against real hardware, and this device holds a
signing key derived from that authenticator.

**And it has signed.** `key test` asked the key for a signature over a random
challenge: it came back, all five checks passed, and the verdict verified against
the derived public key — the one equality nothing on the host side could check.

**And the loop is closed.** A real request went out on the bus, the light went
white, a fingertip on the key made the signature, and `hook.verify_reply` — the
verifier Claude Code would use — called the reply **`TRUSTED`**.

[`status.md`](status.md) is row by row and is the file to trust.

## The documents

[`CLAUDE.md`](CLAUDE.md) is the map and carries what the rest assume — what this
device is in the protocol, what it needs on the LAN, and the rules that may not be
softened. Under it: [`hardware.md`](hardware.md), [`led.md`](led.md),
[`key.md`](key.md), [`protocol.md`](protocol.md), [`firmware.md`](firmware.md),
[`tests.md`](tests.md), [`build.md`](build.md), [`commands.md`](commands.md),
[`status.md`](status.md), [`working-with-code.md`](working-with-code.md).

And one that is for neither arriving nor changing:
[`user-manual.md`](user-manual.md), written for whoever is **holding** the device —
the two sockets, the one light, the gesture that approves and the two that refuse,
with photographs of the real board.

This file is for arriving; those are the authoritative ones.
