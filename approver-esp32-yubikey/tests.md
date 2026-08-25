# The tests (§10.11)

Three tiers, the same three the sibling board has, and the same rule: **the tier
that runs on every change has to be the comprehensive one**, because it is the
only one that will actually be run.

| Tier | Needs | What it pins | Where |
|------|-------|--------------|-------|
| **1 — host** | a C++ compiler | everything with a decision in it and no ESP-IDF under it | `host_test/`, `run.cmd` |
| **2 — parity vectors** | the same, plus Python to *regenerate* | that §7's bytes match `approver/protocol.py` **byte for byte** | `host_test/test_vectors.cpp`, and the header next door |
| **3 — device** | the board, and for §10.18 a security key | everything a fake cannot answer | by hand, and `../scripts/` |

## Tier 1 — the host tests

```
host_test\run.cmd            everything
host_test\run.cmd led fido   only the suites whose name matches
```

**406 tests, 0 failures** as of this writing. `working-with-code.md` has the one
line that runs them and what it needs; `host_test/CMakeLists.txt` explains why
this is a plain CMake project rather than `idf.py --preview set-target linux` (the
short version: that target is offered by this install and does not work on a
Windows host).

### What is under test, and why each one is there

Nineteen suites. Five are this board's own and the rest are shared with the
sibling folder, where they are expected to stay identical.

**This board's own:**

| Suite | Why it exists |
|-------|---------------|
| `led` | the encoding is the one thing whose only other verification method is *looking at a desk*. A single wrong bit is an LED that is the wrong colour, and one of the tests decodes a frame back by hand — because a table whose entries were transposed still emits only the four legal characters |
| `indicator` | a fourteen-way ranking that could otherwise only be checked by unplugging things. It pins the order, the two deliberate rule-breaks in it, and **that no two states look alike** — with one emitter, a shared appearance is a state the operator cannot see |
| `ctaphid` | a sequence number, a length that arrives before the data, and a buffer whose fill rate a device on the other end of a cable controls. A *real* key will never send a malformed frame, which is precisely why the malformed paths need a test |
| `cbor` | every length in a CTAP2 response is a number the key chose. Also the writer: CTAP2 requires canonical CBOR, and a canonicity bug looks like a key that mysteriously will not talk to this device |
| `ctap2` | that the requests are bytes a real key accepts — including **`up: true`, said out loud**, which is the single most important byte this firmware sends — and that a malformed answer never reaches the verifier |

**Shared with the sibling board:** `buttons`, `config`, `timezone`,
`request_card`, `wifi_policy`, `reachability`, `timesync`, `nats`, `signing`,
`registration`, `approval`, `limits`, `activity`, `vectors`.

Three of those changed here and the diffs are worth knowing:

* **`config`** lost seven web tests and gained three about the gate — that
  `requireKey` defaults to **true** when the section is absent, that it moves only
  for a real JSON boolean, and that the three approval fields round-trip. The
  first two are §10.10 as a test: the wrong default and a coerced string are both
  wrong in the direction that matters;
* **`request_card`** no longer ties its queue bound to `Navigator::kMaxPending`,
  because there is no navigator. Four now stands on its own argument;
* **`buttons`** still tests a three-button table even though this board has one,
  because that is what exercises "one press does not move the others".

### What the host tier deliberately cannot reach

`fido_usb.cpp` — enumeration, interface claiming, transfers, and surviving a key
pulled out mid-exchange. There is no fake USB host and there should not be one:
what would be under test is the fake. That is tier 3's, and `status.md` says so.

`led.cpp`'s UART half, `indicator.cpp`'s task, and every driver in `wifi/`,
`nats/` and `storage/` are in the same position and for the same reason.

## Tier 2 — the parity vectors

**There is one copy of `parity_vectors.h` in this repository and it lives next
door**, in `../approver-esp32/host_test/vectors/`. This folder's CMakeLists reads
it from there (`VECTORS_DIR`, overridable) and keeps no copy of its own.

That is deliberate. What the header pins is a *protocol* fact — the bytes §7 signs
— which is identical for both devices by definition, so two committed copies would
be two files that must never differ and nothing that would notice if they did.

The **generator** lives next door too, and a copy of it in this folder was written
and then deleted: `make_vectors.py` resolves its output path from the repository
root, so running it from here would have silently regenerated the *other* board's
files. One generated file, one generator, one place.

It is generated from `approver/protocol.py` and `lib/crypto.py` and **committed**,
so the host build stays a pure C++ one and needs no Python.

What keeps the committed file honest is `tests/test_esp32_vectors.py` on the other
side of the repository, which regenerates it on every `pytest` run and fails if it
differs.

**This is the tier that matters most and it is the smallest.** §7's signing bytes
have to match another language byte for byte; if they disagree by one character,
every reply this device sends is rejected and Claude Code falls back to asking in
its own terminal — which looks exactly like a device that is not answering.

```
..\scripts\make-vectors.cmd --check
```

## Tier 3 — the device

What only a board can answer. **Split here into what has been done and what has
not**, because this device is half-finished in a specific place and a test list
that did not say so would be a list nobody could act on.

### Done, on the board on this desk

| | What was checked |
|---|---|
| boot | comes up on the real board, PSRAM detected as 8 MB octal, storage mounts, `config.json` parses |
| the LED | **found a bug**: `uart_driver_install` refuses a receive buffer at or below 128 bytes, and the first version had zero. The symptom was one line in the boot log and a dark LED |
| the key self-test | `crypto: self-test passed` on every boot, on this chip |
| the console | every command answers on UART0 through the CH343P bridge at 115200 |
| Wi-Fi | joins, gets a DHCP lease, and the reachability check agrees |
| the bus | connects to the real NATS server, subscribes to `status` and `activity`, and `Server max_payload: 65536` comes back |
| SNTP | the clock is set from `pool.ntp.org` — which on a board with no RTC is the only way it is ever right |
| **registration** | a real `register <token>` against the real handler, verified reply, key pinned, `registration.json` written, and the responder went onto `approvals.*` |
| **the queue and the gate's failure path** | `request test 25`: queued, the LED went white and fast, the gate waited, nobody plugged a key in, and it expired with **no reply** and `gate said 1 asked, 0 approved, 0 button-denied, 1 nothing` |
| **the not-enrolled blocker** | with nothing enrolled the device refuses to subscribe and says so, which is §10.10 rule 5 |
| **the spin** | **found a bug**: the first gate refused instantly with no key, nothing decided the request, and the task re-took it several hundred times a second. Fixed in two places (`WaitForKey`, `g_abandoned_nonce`) and re-checked: 700 bytes of console output over 25 seconds of waiting |

### Not done, and each one needs something that is not here

| | What it needs | What it would check |
|---|---|---|
| **the whole of §10.18 against real hardware** | a FIDO security key | enumeration, interface selection on a three-interface YubiKey, `CTAPHID_INIT`, `getInfo`, `makeCredential`, `getAssertion`, and an assertion that actually verifies |
| **the deny button** | a finger | `request test`, tap BOOT, expect a `deny` and a red flash. The button *reads* correctly (`buttons` shows `BOOT GPIO0 released raw released`) — what is untested is the path from a press to a verdict |
| **an end-to-end allow** | both of the above, plus `hook.py` | the only test that matters: a real permission request from Claude Code, answered with a touch, and `hook.verify_reply` calling it `trusted` |
| **a key unplugged mid-exchange** | a key, and a hand | the path `fido_usb.cpp` calls `kUnplugged`, which is the ordinary case rather than an edge one |
| **the restore window** | a finger and a reset | hold BOOT while the board comes up, watch for white, and check `config.json` came back |
| **`term` on this port** | a terminal | whether the linenoise probe is answered here. The sibling board's answer was "no, because the port does not exist yet"; this board has a real UART from power-on and the answer may differ. `console.cpp` says the question is open |
| **the libsodium size comparison** | a build each way | `CONFIG_LIBSODIUM_USE_MBEDTLS_SHA` saved 10,780 bytes on esp32c6. That number is inherited, not measured here |

**The gate's failure paths are better tested than its success path**, which is an
honest description of where this device is and not an accident: everything that
happens when there is *no* key has been run on hardware, and nothing that happens
when there *is* one has.

## What is owed

* run every row of the second table above, once there is a key;
* a `scripts/esp32yk-approval.cmd` to match the sibling's `esp32-approval.cmd` —
  the whole loop against the real hook, with two touches and one deliberate
  non-touch. (`scripts/esp32yk-host-tests.cmd` already exists and is the launcher
  for tier 1);
CI already covers tier 1: `.github/workflows/tests.yml`'s `esp32-host` job builds
**both** boards' host tiers on Windows, in one job — the MSVC setup above them
took three attempts to get right on a moving runner image, and a second job would
be a second copy of it to keep in step. It passes no `-DVECTORS_DIR`, which is the
point: the default is the sibling folder's copy, and a checkout has it.
