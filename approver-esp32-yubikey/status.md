# What is real, row by row

**No decisions in this file** — those live in the documents [`CLAUDE.md`](CLAUDE.md)
maps. This is the fastest-moving file here and the only honest answer to "what
actually works".

Three states, and the middle one is the one to watch:

* **runs** — done on the board on this desk, and observed
* **written** — compiled, host-tested where it can be, **never run against the
  real thing**
* **design** — a document and no code

## The device

| Piece | State | Note |
|-------|-------|------|
| Boot, PSRAM, flash | **runs** | 8 MB octal PSRAM detected, 16 MB flash, 8.5 MB free heap |
| SPIFFS + `config.json` | **runs** | mounts, parses, saves, reloads, restores |
| Ed25519 identity (§10.6) | **runs**, and **signs nothing** | derived at boot, self-test passes. Since §10.18 the verdict is signed by the security key; what libsodium is still for is verifying §6's *reply* |
| — its custody | **moot, and owed** | the seed is still in unencrypted NVS and no longer protects anything. Deleting the identity is owed work (below) |
| ARKG derivation (§10.18.2) | **written** | `components/arkg`, 3,921 bytes. The five pure steps are host-tested against numbers Python produced; **the two curve steps have never run on this chip** — `key selftest` is the command and nobody has typed it |
| Console on UART0 (§10.7) | **runs** | all 16 commands answer |
| WS2812 on GPIO48 (§10.17) | **runs** | UART1 at 3.33 Mbaud, inverted; 0 write failures over thousands of frames |
| The touch prompt (§10.17) | **runs** | blue and fast while `key enrol` / `key test` wait for a fingertip, and gone the moment the key answers rather than waited out |
| The state ranking (§10.17) | **runs** | transitions observed for `booting → no-wifi → no-bus → not-enrolled → not-registered → pending` |
| BOOT button | **partly** | it **reads** (`buttons` is correct). The press → verdict path is untested |
| Wi-Fi (§10.9) | **runs** | joins, DHCP, reachability check |
| NATS (§10.5) | **runs** | connects, subscribes, publishes; reconnect re-subscribes |
| Registration (§10.7) | **ran once, and there is none now** | a real token, real handler, verified reply, key pinned — but it registered an **Ed25519** key, from before §10.18 moved the signer. `registration.json` is no longer on the device (it says so at boot), so the state is `registered no` rather than `STALE`, and the device stays off `approvals.*` until a fresh token registers the **enrolled** key (§10.18.1) |
| Request queue + TTL | **runs** | `request test` queues, lights, expires with no reply |
| The gate's failure paths (§10.18) | **runs** | no key → waits → expires → **no reply**, and no spin |
| The not-enrolled blocker | **runs** | refuses to subscribe, and says why |
| The enrolment→registration ordering | **runs** | with nothing enrolled the light says `not-enrolled`, not `not-registered` — the enrolment is the lower rung because `register` refuses without one (§10.17) |
| Signing + publishing a verdict | **runs** | a real `allow` was signed inside the key and published into the hook's inbox, and `hook.verify_reply` called it `TRUSTED`. The gate's stack was the tightest number on this device and is now measured rather than guessed: the peak is **6,928 bytes**, seen twice (1280 free of 8192, then 5360 free of 12288, agreeing to within 16 bytes). `kGateStackBytes` is 12288 because 15 % headroom on the one path that must not fail is not enough |
| The gate's deadline on a **real** request | **fixed here** | it used `request.ttl_ms` raw, and §7 does not carry the hook's timeout — so every real request got a deadline of *now* and was refused in 3 ms without the key being asked, while `request test` (which always names a TTL) worked. `ui::EffectiveTtlMs` is now the one place that decides, used by the queue and the gate alike |
| The boot report of a stale registration | **fixed here** | `registration::Init()` runs before `fido::Init()` — which is after the bus on purpose — so the check compared against an empty key and called every registration stale. It printed that on the first boot after a good registration, which is an instruction to spend a one-time token for nothing. It is now `registration::ReportKeyBinding()`, called from `main` once the key is up |
| A signed **deny** (§10.18.5) | **runs** | a real request denied on BOOT, signed inside the key on a second touch, published, and `hook.verify_reply` called it `TRUSTED — Claude Code would deny this` (72 bytes). Device side: `1 button-denied, 0 nothing`, `1 replied (0 allow, 1 deny)`. **It took three bugs** — see below — and the "walk away after the tap" path is still the one to check next |
| The deny button being *seen* | **fixed here** | `Debounce` promotes a level that held across two polls, and the gate polled from a hook the key wakes every 100–300 ms — so BOOT had to be held most of a second, and an ordinary tap vanished between two samples. `buttons` now owns a 10 ms poller and latches the press until the gate collects it |
| The light after a tap | **fixed here** | there was none: the tap changed nothing an operator could see, and the next thing they do is touch the key, which signs an `allow`. `deny-pending` is red and it now lasts as long as the wait (4.9 s on the run above) |
| The cancelled request's reply | **fixed here** | a key told `CTAPHID_CANCEL` still answers the request it abandoned, with `0x2D`. Nobody drained it, so the deny's `getAssertion` read the *allow's* reply as its own and died in 14 ms. The deny path had never worked once |

## The key (§10.18)

**This half has met hardware now, and the enrolment is real.** A YubiKey 5
(`1050:0407`, `OTP+FIDO+CCID`, aaguid `f4ce5fc0…`) has been on the OTG port, and on
**2026-08-26** it was enumerated, interrogated and enrolled: this device derived a
signing key from that authenticator's seed key and holds it in `fido.json`. It
signs as `AmHB+df5hQGLvelUF0QGzq/HuWCTKp+/DMie41ByjGvP` (p256).

**And the key has signed.** `key test` was run twice against the real key: an
assertion came back, all five checks of §10.18.3 passed, and the verdict signature
verified against the **derived** public key — which is the one equality this whole
design rests on, and the thing nothing on the host side could have checked for it.
Two DER signatures of different lengths (70 and 71 bytes) both parsed, so PSA's
DER→raw conversion has been exercised on real variable-length output.

**And the loop is closed.** On **2026-08-26** a real request went
`request → white light → touch → signed reply → hook`, and
`hook.verify_reply` — the hook's own verifier, the one Claude Code would act on —
called it **`TRUSTED`**. 71 bytes of ECDSA over §7's own signing bytes, made inside
the security key while somebody was touching it, accepted by the same code path
that judges the four other responders.

Getting there took two bugs that only this test could have found, and both are in
the table below: a gate that refused every real request in three milliseconds, and
a boot line that called a good registration stale.

| Piece | State | Note |
|-------|-------|------|
| USB host: install, daemon, client | **runs** | enumerated a real composite device; `1 attached, 1 claimed, 0 rejected` |
| Interface selection | **runs** | picked interface 1 (`in 0x84, out 0x04`) out of a YubiKey's three. This was the row expected to go wrong first, and it did not |
| CTAPHID framing | **runs** | INIT, a channel, and several CBOR exchanges against a real key with `0 framing` errors. 16 host tests underneath it |
| CBOR | **runs** | parsed a real `getInfo` and a real `makeCredential` response. 20 host tests underneath it |
| CTAP2 requests/responses | **runs** | `getInfo`, `makeCredential` and `getAssertion`, all against the real key |
| `previewSign` — `generateKey` | **runs** | **a real key produced one**, and the draft's shape was read correctly: a 34-byte key handle and a seed key that both parsed. This was the row most likely to be a misreading of the draft |
| `previewSign` — the assertion signature | **runs** | a real key produced one, in the place the draft said it would: inside the authenticator data's extension outputs. Both readings of the draft — `generateKey` and this — turned out right |
| The five checks (§10.18.3) | **run, and passed** | rp hash, user presence, credential match, the assertion's own ECDSA, and the verdict's against the derived key. The last one is the equality the design rests on: the key this chip derived is one the authenticator can reconstruct the private half of |
| The `previewSign` advertisement check | **runs** | `getInfo`'s extension list is parsed, `key info` prints it, and `key enrol` refuses a key that does not advertise it rather than spending a touch on a `makeCredential` that fails with a status naming no cause |
| `key info` / `key enrol` | **run** | both against the real key |
| `key test` | **runs** | twice: 7.2 s and 17.1 s, both `approved`, both verified. The seconds are the human walking over |
| `key selftest` | **runs** | on the chip, against Python's vector, in **661–670 ms** |
| `fido.json` (format 2) | **runs** | written by a real enrolment, and re-read at the next boot into the same `signs as` — including across an `app-flash` |
| PSA ECDSA verification | **runs** | the DER→raw conversion, against real signatures of 70 **and** 71 bytes — the variable length that made it the part most likely to be wrong first |
| The registration↔enrolment binding | **partly** | the boot comparison runs. The **stale** path has not been seen, because there is no `registration.json` on the device to be stale |

## Owed, and known

| | Why it is owed |
|---|---|
| **`key selftest` on the board** | it needs nothing at all and would move the ARKG derivation from "written" to "runs on this chip". Until it is typed, the two curve operations in `arkg_psa.cpp` are unrun code |
| **Deleting the Ed25519 identity** | since §10.18 it signs nothing, and what is left of `components/crypto` — `Verify` for §6's reply, and base64 — needs no seed. Removing it takes the last private key off this board's flash, along with `keys forget now`, one blocker in the responder that can never fire, and a `device_key` input to the light |
| **A `previewSign` capability check at enrolment** | `key info` reads the extension list and nothing refuses an enrolment on a key that lacks `previewSign`. Today that fails one step later, with a message naming the likely cause — which is honest but is a round trip and a touch too late |

## Not built at all

| | Why |
|---|---|
| **Any display at all** | there is no panel on this board (§10.1), so nothing renders anything — and nothing subscribes to a subject whose only consumer would be a readout. One WS2812 is the whole interface (§10.17) |
| **A clock** | no RTC (no I²C bus for one) and no SNTP: §7's `ts` is echoed from the request, nothing else here reads a wall-clock time, and there is nowhere to show one (§10.13) |
| **A web configuration site** | the way in on this board is the CH343P bridge and the console on it, which is a socket rather than a network service. A second surface would be one more thing to keep away from a verdict (§10.10 rule 4) |
| **OTA** | the partition table has two slots and nothing uses them |
| **A user manual** | one document for whoever is *holding* the device rather than changing it: the light, the two gestures, and first-time setup. When the key works, this is the document to write |
| **`scripts/esp32yk-approval.cmd`** | the end-to-end script. [`tests.md`](tests.md) lists it as owed — `scripts/esp32yk-host-tests.cmd` exists and covers tier 1 |

## Known differences from the committed tree

| | |
|---|---|
| **The device's `config.init.json` is older than the repository's** | a full `idf.py flash` would fix it and would cost the registration, the Wi-Fi passphrase and the enrolment (`build.md`). It only matters on a restore |
| **The device's `config.json` holds a real Wi-Fi passphrase** | the committed one holds `YOUR_SSID` / `CHANGEME` placeholders, and §10.15's rule is that a passphrase is a secret from the moment it is typed. Nothing in this repository carries it |

## Two bugs this device found in itself

Recorded because both were found by running the thing rather than by reading it,
and both are the kind that a test could not have caught first.

**1. The LED did not come up.** `uart_driver_install` refuses a receive buffer at
or below the 128-byte hardware FIFO, and the first version passed zero — nothing
here ever reads. The symptom was one `E` line in a boot log and a dark LED. The
fix is 256 bytes of RAM spent to satisfy a check, and `led.cpp` says so at the
constant.

**2. The gate spun.** With no key on the port the first version returned
`kNoKey` in microseconds; nothing had decided the request, so it stayed at the
head of the queue and the gate task took it again — several hundred times a
second, each with a log line, until the TTL ran out. It looked like a crash and
was a fail-safe failing loudly enough to drown the console. Fixed in two places:
`WaitForKey` (wait for a key rather than refusing at the door, which also makes
"plug it in while the request is up" work) and `g_abandoned_nonce` (a request the
gate gave up on is not re-gated).

There is also **one bug this device found in the *design***, which is
`responder::Blocker::kNotEnrolled`: on its first real registration it went
straight onto `approvals.*` with nothing enrolled and began taking requests it
could not answer out of a shared queue group. That is §10.10 rule 5, and it did
not exist until the device broke it.
