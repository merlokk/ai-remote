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
| The state ranking (§10.17) | **runs** | transitions observed for `booting → no-wifi → no-bus → not-registered → not-enrolled → pending` |
| BOOT button | **partly** | it **reads** (`buttons` is correct). The press → verdict path is untested |
| Wi-Fi (§10.9) | **runs** | joins, DHCP, reachability check |
| NATS (§10.5) | **runs** | connects, subscribes, publishes; reconnect re-subscribes |
| Registration (§10.7) | **ran, and is now stale** | a real token, real handler, verified reply, key pinned — but it registered an **Ed25519** key, and this firmware signs with the enrolled P-256 one. The device says `registered STALE` and stays off the subject until `key enrol` + a fresh token (§10.18.1) |
| Request queue + TTL | **runs** | `request test` queues, lights, expires with no reply |
| The gate's failure paths (§10.18) | **runs** | no key → waits → expires → **no reply**, and no spin |
| The not-enrolled blocker | **runs** | refuses to subscribe, and says why |
| Signing + publishing a verdict | **written** | and it is no longer the sibling board's code: the signature comes out of the key and this device only base64s it and publishes. **Nothing has ever got past the gate here**, so it has never run |
| A signed **deny** (§10.18.5) | **written** | the button chooses it and the key has to sign it — a second touch. Never run, and the "walk away after the tap" path (no reply) is the one to check first |

## The key (§10.18)

**This is the half that has never met hardware — and since §10.18 it is also the
half that holds the signing key.** No FIDO authenticator has been plugged into the
OTG port, so this device currently has no way to sign anything at all.

| Piece | State | Note |
|-------|-------|------|
| CTAPHID framing | **written** | 16 host tests, including every malformed path a real key would never produce |
| CBOR | **written** | 20 host tests, including the hostile shapes |
| CTAP2 requests/responses | **written** | 20 host tests against hand-built bytes |
| `previewSign` on the wire | **written** | both requests and both answers, host-tested against responses the vector generator builds in the draft's shape. **No real key has produced one** — those bytes are one reading of a draft, and the first thing to compare a real answer against |
| The five checks (§10.18.3) | **written** | rp hash, user presence, credential match, the assertion's own ECDSA, and the verdict's against the derived key |
| USB host: install, daemon, client | **partly** | `usb_host_install` succeeds at boot and the two tasks run. **Nothing has ever been enumerated** |
| Interface selection | **written** | the endpoint-shape heuristic, unrun. A YubiKey has three interfaces and this is where it would first go wrong |
| `key info` / `key enrol` / `key test` | **written** | the console commands exist and answer "nothing on the OTG port" |
| `key selftest` | **written, and runnable today** | needs no key. It is the only row in this table that can move without hardware |
| `fido.json` (format 2) | **written** | load, save, forget, and the boot re-derivation that checks the file still produces the registered key. Never written by a real enrolment |
| PSA ECDSA verification | **written** | the DER→raw conversion is the part most likely to be wrong first |
| The registration↔enrolment binding | **written** | `registration.json` records the key it was made for and the two are compared at boot (§10.18.1). Never exercised by a real re-enrolment |

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
