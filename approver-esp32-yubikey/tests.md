# The tests (§10.11)

Three tiers, the same three the sibling board has, and the same rule: **the tier
that runs on every change has to be the comprehensive one**, because it is the
only one that will actually be run.

| Tier | Needs | What it pins | Where |
|------|-------|--------------|-------|
| **1 — host** | a C++ compiler | everything with a decision in it and no ESP-IDF under it | `host_test/`, `run.cmd` |
| **2 — parity vectors** | the same, plus Python to *regenerate* | that §7's bytes match `approver/protocol.py` **byte for byte**, and that the ARKG derivation matches an independent implementation of the draft | `host_test/test_vectors.cpp` + `test_arkg.cpp`, and two generated headers |
| **3 — device** | the board, and for §10.18 a security key | everything a fake cannot answer | by hand, and `../scripts/` |

## Tier 1 — the host tests

```
host_test\run.cmd            everything
host_test\run.cmd led fido   only the suites whose name matches
```

**358 tests, 0 failures** as of this writing. `working-with-code.md` has the one
line that runs them and what it needs; `host_test/CMakeLists.txt` explains why
this is a plain CMake project rather than `idf.py --preview set-target linux` (the
short version: that target is offered by this install and does not work on a
Windows host).

### What is under test, and why each one is there

Seventeen suites. Six are this board's own, ten are shared with the sibling folder
— where they are expected to stay identical — and one is a formatter the console
uses.

**This board's own:**

| Suite | Why it exists |
|-------|---------------|
| `led` | the encoding is the one thing whose only other verification method is *looking at a desk*. A single wrong bit is an LED that is the wrong colour, and one of the tests decodes a frame back by hand — because a table whose entries were transposed still emits only the four legal characters |
| `indicator` | a fifteen-way ranking that could otherwise only be checked by unplugging things. It pins the order, the two deliberate rule-breaks in it, and **that no two states look alike** — with one emitter, a shared appearance is a state the operator cannot see |
| `ctaphid` | a sequence number, a length that arrives before the data, and a buffer whose fill rate a device on the other end of a cable controls. A *real* key will never send a malformed frame, which is precisely why the malformed paths need a test |
| `cbor` | every length in a CTAP2 response is a number the key chose. Also the writer: CTAP2 requires canonical CBOR, and a canonicity bug looks like a key that mysteriously will not talk to this device |
| `ctap2` | that the requests are bytes a real key accepts — including **`up: true`, said out loud**, which is the single most important byte this firmware sends — and that a malformed answer never reaches the verifier |
| `arkg` | **the derivation that decides which key signs a verdict** (§10.18.2), and the `previewSign` shapes around it. The biggest suite here, and the one whose failure mode is silence: a derivation wrong by one byte registers a public key whose private half no authenticator can reconstruct, every reply is rejected by the hook, and from the desk that looks exactly like a device that is not answering |

**Shared with the sibling board:** `buttons`, `config`, `request_card`,
`wifi_policy`, `reachability`, `nats`, `signing`, `registration`, `approval`,
`vectors`.

**And one small one:** `age_text` — the duration formatter `cli/console.cpp` prints
ages with. `test_age_text.cpp` keeps its three bands pinned so two readouts cannot
describe one instant two different ways.

### The `arkg` suite, and what it can and cannot reach

Worth its own paragraph, because the split inside it is unusual and deliberate.

**The curve is not in the host build.** `components/arkg` takes its four
primitives — SHA-256, ECDH, `scalar * G`, point addition — through a `Backend` of
function pointers, and the suite supplies one that *replays* the two curve steps
from the generated vectors: it recognises the exact scalars and points Python
computed and hands back Python's answers. Everything between them is the shipping
code doing real work on real numbers.

So five of the derivation's seven steps are covered here, byte for byte, with every
intermediate checked rather than just the answer — `expand_message_xmd`, two HKDF
expansions, an HMAC truncated to 128 bits, a reduction modulo the subgroup order,
and the label assembly that scopes all of it. That is what makes a failure name the
step instead of reporting "the key is wrong".

**And it is honest about what it leaves out**: a backend whose ECDH is wrong passes
every test in this file. That half is `key selftest` on the device (tier 3), which
runs the same vector through PSA and mbedTLS on the chip. It needs no security key,
so it is the one part of §10.18 that can be checked on the board today.

The suite also covers `previewSign` on the wire — the two requests this device
builds, and the two answers it reads — against responses the generator builds in
the shape the draft describes. Nobody here has seen one on hardware, so writing
those bytes by hand in C++ would have pinned one reading of the draft twice.

**There is no suite here for hardware this board does not have**, and none is kept
as a placeholder for one — a clock, an I²C bus and the chips on it have nothing to
point a test at (§10.13).

Three of the shared suites differ here and the diffs are worth knowing:

* **`config`** has three tests about the gate — that a file still carrying
  `requireKey` loads without it and does not write it back (§10.18.6), that
  `denyButton` moves only for a real JSON boolean, and that the approval fields
  round-trip. The first is the one with teeth: a stale config must not be read as
  permission by a firmware that no longer has such a mode;
* **`request_card`** ties its queue bound to nothing but its own argument — four
  pending requests, because that is what four 2.3 KB slots cost;
* **`buttons`** still tests a three-button table even though this board has one,
  because that is what exercises "one press does not move the others".

### What the host tier deliberately cannot reach

`fido_usb.cpp` — enumeration, interface claiming, transfers, and surviving a key
pulled out mid-exchange. There is no fake USB host and there should not be one:
what would be under test is the fake. That is tier 3's, and `status.md` says so.

`led.cpp`'s UART half, `indicator.cpp`'s task, and every driver in `wifi/`,
`nats/` and `storage/` are in the same position and for the same reason.

## Tier 2 — the parity vectors

**Two generators now, and the split is by what the bytes belong to.**

### §7's bytes: the sibling board's file, read from here

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

### The ARKG derivation: this folder's own, and the argument for a second file

`host_test/vectors/arkg_vectors.h` and `components/arkg/arkg_selftest_vector.h`
are generated by `tools/make_arkg_vectors.py` and committed **here**, and the rule
above does not apply to them for a reason: what they pin is not a protocol fact
shared by both devices. The C6 board has no ARKG in it at all.

```
..\scripts\esp32yk-make-vectors.cmd --check
```

What is in them: the seed key, three derivations with **every intermediate**
(including a `ctx` at the draft's 64-byte ceiling and an empty one, which is where
the length prefix shows up), the `expand_message_xmd` cases underneath, the
COSE_Sign_Args map, and two synthetic CTAP2 responses in `previewSign`'s shape. The
second header is the small one that ships **inside the firmware**, for `key
selftest`.

**Three implementations, and the one that decides is not in this repository.** The
generator is a second, independent implementation of ARKG-P256ADD-ECDH written from
the draft over `cryptography` alone — deliberately *not* over `fido2`, so a bare
`uv sync` can regenerate. Where the `yubikey` extra is installed,
`tests/test_esp32yk_arkg_vectors.py` additionally cross-checks the endpoint against
**Yubico's own** implementation, which is the agreement that matters: the private
half of the derived key is reconstructed by their code inside the authenticator, so
if we and they disagree, the device signs with a key nobody registered.

That test also runs the whole chain in Python, minus the hardware: derive, take the
compressed point, sign §7's bytes with the matching private scalar, and check
`lib.crypto.verify` accepts it under `key_type="p256"`. It is the closest thing
there is to an end-to-end test until a key is plugged in.

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
| the bus | connects to the real NATS server, subscribes to `approvals.*` in the `approvers` queue group, and `Server max_payload: 65536` comes back |
| **registration** | a real `register <token>` against the real handler, verified reply, key pinned, `registration.json` written, and the responder went onto `approvals.*` |
| **the queue and the gate's failure path** | `request test 25`: queued, the LED went white and fast, the gate waited, nobody plugged a key in, and it expired with **no reply** and `gate said 1 asked, 0 approved, 0 button-denied, 1 nothing` |
| **the not-enrolled blocker** | with nothing enrolled the device refuses to subscribe and says so, which is §10.10 rule 5 |
| **the enrolment→registration ordering** | with neither done the light says `not-enrolled`, not `not-registered` — `register` refuses without an enrolment, so the other order sends an operator to spend a one-time token they cannot use |
| **`key selftest` on the chip** | the ARKG derivation's two curve steps against the committed vector: **agrees with Python, 661–670 ms** |
| **enumeration and enrolment against a real key** | a YubiKey 5 (`1050:0407`, `OTP+FIDO+CCID`) on the OTG port: claimed on interface 1 of three, `CTAPHID_INIT`, `getInfo`, and a `makeCredential` with `generateKey` that came back with a 34-byte key handle and a seed key the device derived its signing key from. `fido.json` written, and re-read into the same key across a reflash |
| **a touch that never came** | the key answers `CTAP 0x27`, nothing is enrolled, and nothing is published. It cost two attempts to recognise, which is why the console now names the cause instead of printing the transport's `ok` |
| **an assertion, and the equality under the whole design** | `key test`, twice: `getAssertion` with `previewSign`, all five checks of §10.18.3, and the verdict signature verifying against the **derived** public key — so the key this chip derived is one the authenticator can reconstruct the private half of. Two DER signatures of different lengths (70, 71 bytes) both parsed, which is PSA's DER→raw on real variable-length output |
| **an end-to-end allow — the whole point** | a real request from `tools/test_request.py` (the same bytes `hook.py` sends): white light, a touch, an ECDSA signature made inside the key, published into the hook's inbox, and `hook.verify_reply` answering **`TRUSTED`**. Device side: `gate said 1 asked, 1 approved`, `on wire 1 replied (1 allow)`. **It found two bugs first** — see `status.md` — and neither could have been found by any other test: `request test` always names a TTL, so it never took the path a real request takes |
| **a signed deny, end to end** | a real request denied on BOOT: the tap cancelled the key's request for an `allow`, the light went red for the 4.9 s it took to reach the key, the key signed the `deny`, and `hook.verify_reply` called it `TRUSTED — Claude Code would deny this`. Three bugs stood in the way and all three were invisible to the other two tiers: a tap the gate could not sample, a light that never changed, and the cancelled request's own reply being read by the next exchange |
| **walking away after the tap** | BOOT tapped, the light went red, and then nothing: the deny went **unsigned and unpublished**, `0 replied`, counted as `nothing` and not as a deny (§10.10 rule 2), and the hook fell back to its own prompt. The half that has to keep not working |
| **the light after a gate gives up** | with a tap and without one: once the gate stops asking, the light goes back to `ready` instead of flashing white for the rest of the request's TTL. Checked by eye on the board, which is the only instrument that could have found it |
| **the touch prompt** | blue and fast while a console command waits, gone the moment the key answers rather than waited out (§10.17) |
| **the `previewSign` advertisement check** | `getInfo`'s extension list is parsed and `key enrol` refuses a key without it before spending a touch. The key on this desk **does** advertise it, so what is proven here is the parse and the readout, not the refusal |
| **the spin** | **found a bug**: the first gate refused instantly with no key, nothing decided the request, and the task re-took it several hundred times a second. Fixed in two places (`WaitForKey`, `g_abandoned_nonce`) and re-checked: 700 bytes of console output over 25 seconds of waiting |

### Not done, and each one needs something that is not here

| | What it needs | What it would check |
|---|---|---|
| **a key that does not advertise `previewSign`** | a second, ordinary key | the refusal path in `key enrol`. The key on this desk advertises it, so only the readout has been seen |
| **an allow from Claude Code itself** | a live session with the §7 hook wired up | the last step: the request coming from a real `PermissionRequest` rather than from the probe that sends the same bytes |
| **a key unplugged mid-exchange** | a key, and a hand | the path `fido_usb.cpp` calls `kUnplugged`, which is the ordinary case rather than an edge one |
| **the restore window** | a finger and a reset | hold BOOT while the board comes up, watch for white, and check `config.json` came back |
| **`term` on this port** | a terminal | whether the linenoise probe is answered here. The sibling board's answer was "no, because the port does not exist yet"; this board has a real UART from power-on and the answer may differ. `console.cpp` says the question is open |
| **the libsodium size comparison** | a build each way | `CONFIG_LIBSODIUM_USE_MBEDTLS_SHA` saved 10,780 bytes on esp32c6. That number is inherited, not measured here |

**The gate's failure paths are better tested than its success path**, which is an
honest description of where this device is and not an accident: everything that
happens when there is *no* key has been run on hardware, and nothing that happens
when there *is* one has.

**And since §10.18 the registration on that board is stale by construction** — it
names an Ed25519 key this firmware no longer signs with. The first three things to
do when a key arrives are `key selftest`, `key enrol`, `register <token>`, in that
order, and the device says so itself at every step.

## What is owed

* **`key selftest` on the board**, which needs nothing and is owed now rather than
  when a key arrives;
* run every other row of the second table above, once there is a key;
* a `scripts/esp32yk-approval.cmd` to match the sibling's `esp32-approval.cmd` —
  the whole loop against the real hook, with two touches and one deliberate
  non-touch. (`scripts/esp32yk-host-tests.cmd` already exists and is the launcher
  for tier 1);
CI already covers tier 1: `.github/workflows/tests.yml`'s `esp32-host` job builds
**both** boards' host tiers on Windows, in one job — the MSVC setup above them
took three attempts to get right on a moving runner image, and a second job would
be a second copy of it to keep in step. It passes no `-DVECTORS_DIR`, which is the
point: the default is the sibling folder's copy, and a checkout has it.
