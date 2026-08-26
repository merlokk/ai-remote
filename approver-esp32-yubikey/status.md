# What is real, row by row

**No decisions in this file** — those live in the documents [`CLAUDE.md`](CLAUDE.md)
maps. This is the fastest-moving file here and the only honest answer to "what
actually works".

States, and `written` is the one to watch:

* **runs** — done on the board on this desk, and observed
* **partly** — some of it observed and the rest not, with the row saying which
* **written** — compiled, host-tested where it can be, **never run against the
  real thing**
* **design** — a document and no code

**As of 2026-08-26 the tables below are almost entirely `runs`**, which they were
not that morning: the whole of §10.18 was `written` and no security key had ever
been plugged in. What is left is at the bottom, under **Owed, and known**, and the
largest item there is a *removal*.

## The device

| Piece | State | Note |
|-------|-------|------|
| Boot, PSRAM, flash | **runs** | 8 MB octal PSRAM detected, 16 MB flash, 8.5 MB free heap |
| SPIFFS + `config.json` | **runs** | mounts, parses, saves, reloads, restores |
| Ed25519 identity (§10.6) | **deleted** | there is no key of this device's own. What is left of libsodium verifies §6's *reply* — one call — and does base64; a fresh registration on the board proves it still works |
| — its custody | **settled by removal** | the seed was 32 bytes of private key in unencrypted NVS protecting nothing. `crypto::Init` erases it, and did: one `W crypto: erased the 32-byte Ed25519 seed…` on this board. **There is now no private key on this flash of any kind** |
| The verifier self-test | **runs** | a real signature verifies and a one-bit-flipped one does not. A failure keeps the device off `approvals.*` (`Blocker::kCannotVerify`), because a board that cannot check §6's reply cannot know whose key it pinned |
| ARKG derivation (§10.18.2) | **runs** | `components/arkg`, 3,921 bytes. The five pure steps are host-tested against numbers Python produced, and the two curve steps agree with Python **on this chip** — `key selftest`, 661–670 ms |
| Console on UART0 (§10.7) | **runs** | all 16 commands answer |
| WS2812 on GPIO48 (§10.17) | **runs** | UART1 at 3.33 Mbaud, inverted; 0 write failures over thousands of frames |
| `pending` ending with the gate (§10.17) | **runs** | the light stops asking when the gate does, not when the request expires — those are 30 s apart, and the difference was half a minute of white asking for a fingertip with nowhere to put it |
| The touch prompt (§10.17) | **runs** | blue and fast while `key enrol` / `key test` wait for a fingertip, and gone the moment the key answers rather than waited out |
| The state ranking (§10.17) | **runs** | transitions observed for `booting → no-wifi → watching → ready → pending → deny-pending → signing → ready`, and `not-enrolled → not-registered` on a fresh device |
| BOOT button | **runs** | a tap becomes a signed `deny` on the wire. `buttons watch` measures real presses at **70–180 ms**, which is why the sampling had to change — see the bugs below |
| Wi-Fi (§10.9) | **runs** | joins, DHCP, reachability check |
| NATS (§10.5) | **runs** | connects, subscribes, publishes; reconnect re-subscribes |
| Registration (§10.7) | **runs** | a real token, real handler, verified reply, handler key pinned and compared by eye — and it registers the **enrolled** key, so `handler-config.json` holds the same `p256` point the device prints as `signs as`. Took 1.12 s and survives a reflash |
| Request queue + TTL | **runs** | `request test` queues, lights, expires with no reply |
| The gate's failure paths (§10.18) | **runs** | no key → waits → expires → **no reply**, and no spin. Nobody touching → the key's own refusal → no reply. A tap and then nothing → no reply, counted as `nothing` and never as a deny (§10.10 rule 2) |
| The not-enrolled blocker | **runs** | refuses to subscribe, and says why |
| The enrolment→registration ordering | **runs** | with nothing enrolled the light says `not-enrolled`, not `not-registered` — the enrolment is the lower rung because `register` refuses without one (§10.17) |
| Signing + publishing a verdict | **runs** | a real `allow` was signed inside the key and published into the hook's inbox, and `hook.verify_reply` called it `TRUSTED`. The gate's stack was the tightest number here and is now measured rather than guessed: the peak is **6,928 bytes**, seen twice (1280 free of 8192, then 5360 free of 12288, agreeing to within 16). `kGateStackBytes` is 12288 — 15 % headroom on the one path that must not fail is not enough, and it has to come from internal RAM because a task stack may not live in PSRAM (§10.13) |
| A signed **deny** (§10.18.5) | **runs** | a real request denied on BOOT, signed inside the key on a second touch, published, and `hook.verify_reply` called it `TRUSTED — Claude Code would deny this` (72 bytes). Device side: `1 button-denied`, `1 replied (0 allow, 1 deny)`. It took three bugs, all below |

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

Getting there took bugs that only a real request could have found — a gate that
refused every one of them in three milliseconds among them. They are listed under
**What running it found**, below.

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
| The registration↔enrolment binding | **partly** | the boot comparison runs against a real registration and stays quiet, which is the right answer. The **stale** path — a registration naming a key that is no longer enrolled — has still never been produced, because that needs a re-enrolment |

## Owed, and known

| | Why it is owed |
|---|---|
| **A key that does not advertise `previewSign`** | the refusal is written and `key info` prints the line, but the key on this desk advertises it, so only the readout has been seen. Needs a second, ordinary key |
| **A re-enrolment, to see `STALE`** | the binding of §10.18.1 is checked at every boot and has only ever agreed. Producing a disagreement costs an enrolment and then a fresh token |
| **An allow from Claude Code itself** | every request so far came from `tools/test_request.py`, which sends the bytes `hook.py` sends. What has not happened is the request arriving from a live session's `PermissionRequest` |
| **`scripts/esp32yk-approval.cmd`** | the end-to-end script. Everything it would drive now works by hand |

## Not built at all

| | Why |
|---|---|
| **Any display at all** | there is no panel on this board (§10.1), so nothing renders anything — and nothing subscribes to a subject whose only consumer would be a readout. One WS2812 is the whole interface (§10.17) |
| **A clock** | no RTC (no I²C bus for one) and no SNTP: §7's `ts` is echoed from the request, nothing else here reads a wall-clock time, and there is nowhere to show one (§10.13) |
| **A web configuration site** | the way in on this board is the CH343P bridge and the console on it, which is a socket rather than a network service. A second surface would be one more thing to keep away from a verdict (§10.10 rule 4) |
| **OTA** | the partition table has two slots and nothing uses them |
| **A user manual** | one document for whoever is *holding* the device rather than changing it: the light, the two gestures, and first-time setup. It was waiting on the key working, and the key works — so this is now simply owed, and the sibling board's `user-manual.md` is the shape to copy |

## Known differences from the committed tree

| | |
|---|---|
| **The device's `config.init.json` is older than the repository's** | a full `idf.py flash` would fix it and would cost the registration, the Wi-Fi passphrase and the enrolment (`build.md`). It only matters on a restore |
| **The device's `config.json` holds a real Wi-Fi passphrase** | the committed one holds `YOUR_SSID` / `CHANGEME` placeholders, and §10.15's rule is that a passphrase is a secret from the moment it is typed. Nothing in this repository carries it |

## What running it found

**Every defect on this page was found by running the device, and none of them could
have been found any other way.** The host tier has no light, no clock, no finger and
no key; the vector tier has no time in it at all. That is the argument for tier 3
written as evidence rather than as a claim, and it is why this section is the longest
one here.

### Closing the loop: the allow

**The gate refused every real request in three milliseconds.** §7 does not carry the
hook's timeout — `ParseApprovalRequest` leaves `ttl_ms` at zero and says so — and the
queue knew that, substituting its default. The gate did not: it took the field raw,
computed a deadline of *now*, and returned `kTimeout` without ever asking the key.
From the desk it looked perfect, because the queue's own TTL kept the light on for
the full minute; `key` giving `gate 0 asked` is what gave it away. `ui::EffectiveTtlMs`
is the one place that decides now. **`request test` always names a TTL**, which is
why every test ever run on this device took the one path a real request never takes.

**A good registration was reported stale at boot.** `registration::Init()` runs
before `fido::Init()` — which is after the bus on purpose — so the §10.18.1
comparison ran against an empty enrolled key and called every registration stale. It
printed that on the first boot after a *correct* registration, which is an
instruction to spend a one-time token for nothing. Now
`registration::ReportKeyBinding()`, called from `main` once the key is loaded.

### Closing the loop: the deny

Three, and the deny path had never once worked.

**The tap could not be seen.** `Debounce` promotes a level that has held *across* two
polls, and the gate polls from a hook the key wakes every 100–300 ms — it spends the
whole of a request blocked inside a USB read. Real presses measure 70–180 ms, so they
were invisible by construction. `buttons` owns a 10 ms poller now and latches the
press until the gate collects it. Polling faster from inside the USB read was tried
first and is not available: `ReadPacket` re-submits an in-flight transfer, which
becomes `the key could not be reached` sixty milliseconds in.

**Nothing changed when the button was pressed.** The light went on flashing white,
and the next thing an operator does is touch the key — which signs an `allow`. Twice
that is exactly what happened, and both times it looked like a broken button.
`deny-pending` is a state now (§10.17).

**The cancelled request's reply poisoned the next one.** A key told `CTAPHID_CANCEL`
still answers the request it abandoned, with `0x2D`. Nobody drained it, so the deny's
`getAssertion` read the *allow's* reply as its own: fourteen milliseconds,
`Gate::kCancelled`, and a red light that existed for seventeen. And the first drain
was too easily satisfied — a `CTAPHID_KEEPALIVE` is a complete message, so it could
swallow one, report success and leave the reply where it was. It skips them now.

### Lights and readouts that said the wrong thing

None of these stopped a verdict. All of them sent somebody the wrong way.

* **`pending` outlasted the gate by thirty seconds.** The gate's ceiling is 30 s and
  the request's TTL is a minute, and in between the light went on asking for a
  fingertip that no longer had anywhere to go. Found by watching the board: the key
  stopped blinking and the white kept going;
* **the ranking sent operators to spend a token they could not use** — `registered`
  was checked before `fido_enrolled`, while `register` refuses without an enrolment;
* **`keys` printed the sibling board's `key_id`**, from a string literal, in the one
  command an operator reads to find out what the device is;
* **`status`'s `heap` counted the PSRAM**, so it read as eight and a half megabytes
  and said nothing about the memory that is actually scarce — the memory task stacks
  come out of, and the memory that decided `kGateStackBytes` an hour later;
* **a failure said `ok`.** A key that refuses leaves the transport at `Fault::kNone`,
  whose name is "ok", so the line reporting the failure began with that word and the
  CTAP status holding the cause read like an aside. `CTAP 0x27` — what a key answers
  when nobody touched it — cost two attempts to read correctly;
* **`CHANNEL_BUSY` read as "the key could not be reached"**, and two runs went into
  chasing a firmware bug. Reflash the board while a request waits for a fingertip and
  the key holds a transaction for a channel with nobody on it; unplugging it is the
  fix, and the log says so now.

### And one the documents had already promised

**The `previewSign` capability check existed only in a comment.** `ctap2.h` stated
that a key not advertising it "cannot be enrolled on this device at all, and
`key info` is where that gets reported" — while `Info` held no extension list,
`ParseInfo` never read key 2, and `ParseInfo` had no host tests at all. Found by an
enrolment failing for a reason the readout could have named in advance.

## Two bugs from before this all worked

Recorded because both were found the same way, and because the fixes are load-bearing
in what runs today.

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
