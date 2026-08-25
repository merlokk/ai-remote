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
| Ed25519 identity (§10.6) | **runs** | derived at boot, self-test passes on this chip |
| — its custody | **fallback** | the seed is in **unencrypted NVS**, not behind an eFuse. §10.6 has the table; §10.18 is what makes it a smaller problem here |
| Console on UART0 (§10.7) | **runs** | all 18 commands answer |
| WS2812 on GPIO48 (§10.17) | **runs** | UART1 at 3.33 Mbaud, inverted; 0 write failures over thousands of frames |
| The state ranking (§10.17) | **runs** | transitions observed for `booting → no-wifi → no-bus → not-registered → not-enrolled → pending` |
| BOOT button | **partly** | it **reads** (`buttons` is correct). The press → verdict path is untested |
| Wi-Fi (§10.9) | **runs** | joins, DHCP, reachability check |
| Clock | **runs (SNTP only)** | there is no RTC on this board (§10.13); a power cut loses the time |
| NATS (§10.5) | **runs** | connects, subscribes, publishes; reconnect re-subscribes |
| Registration (§10.7) | **runs** | real token, real handler, reply verified, key pinned |
| `status` / `activity` watch (§9.7, §9.10) | **runs** | subscribed on this device; no documents seen yet because no session has published while it was up |
| Request queue + TTL | **runs** | `request test` queues, lights, expires with no reply |
| The gate's failure paths (§10.18) | **runs** | no key → waits → expires → **no reply**, and no spin |
| The not-enrolled blocker | **runs** | refuses to subscribe, and says why |
| Signing + publishing a verdict | **written** | the code is the sibling board's, which does this daily. **On this device it has never had a verdict to sign**, because nothing has got past the gate |

## The key (§10.18)

**This is the half that has never met hardware.** No FIDO authenticator has been
plugged into the OTG port.

| Piece | State | Note |
|-------|-------|------|
| CTAPHID framing | **written** | 16 host tests, including every malformed path a real key would never produce |
| CBOR | **written** | 20 host tests, including the hostile shapes |
| CTAP2 requests/responses | **written** | 20 host tests against hand-built bytes |
| The four checks (§10.18.2) | **written** | rp hash, user presence, credential match, ECDSA verify |
| USB host: install, daemon, client | **partly** | `usb_host_install` succeeds at boot and the two tasks run. **Nothing has ever been enumerated** |
| Interface selection | **written** | the endpoint-shape heuristic, unrun. A YubiKey has three interfaces and this is where it would first go wrong |
| `key info` / `key enrol` / `key test` | **written** | the console commands exist and answer "nothing on the OTG port" |
| `fido.json` | **written** | load, save, forget; never written by a real enrolment |
| PSA ECDSA verification | **written** | the DER→raw conversion is the part most likely to be wrong first |

## Not built at all

| | Why |
|---|---|
| **A web configuration site** | the sibling board has one (§10.16). Not carried over: it is 2,600 lines and its whole purpose is a way in when there is no other, which on this board is what the CH343P bridge is. It would also be a second surface to keep away from a verdict (§10.10 rule 4) |
| **OTA** | the partition table has two slots and nothing uses them |
| **A user manual** | the sibling folder has one with photographs. This device has no screens to photograph and one light; when the key works, this is the document to write |
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
