# The key (§10.18)

**What has to happen between an operator deciding and a verdict existing.**

## 10.18 The key is the signer

**The verdict's signature is made inside the security key, and nowhere else.**
This device holds no private key that can produce one: what it holds is a
*public* key it derived, the handle that lets the authenticator rebuild the
private half, and a credential that proves somebody touched the thing.

The scheme is §8's — **ARKG over the CTAP2 `previewSign` extension**, the same
one `approver/responder_yubikey.py` uses, and the reason it is the same one is
that `hook.py`, `protocol.py` and `registration_handler.py` must not know this
device exists. What §6 registers is a P-256 public key (`key_type: "p256"`), what
§7 carries is an ECDSA signature over the decision bytes, and `lib/crypto.py` has
verified exactly that pair since §8.7. **Nothing on the host side changed for
this device**, which is the test this design had to pass.

Four properties fall out, and they are the whole reason this folder exists next
to `approver-esp32/`:

* **a touch cannot be replayed onto a different request.** What the key signs is
  `SHA-256(protocol::DecisionSigningBytes)` — the session, the nonce, the tool,
  the input hash, *the verdict* and the timestamp. A different request is a
  different digest and needs a different touch;
* **the device alone cannot approve anything, and this is now literal.** Firmware
  that had been tampered with does not have a signing key to abuse. There is no
  seed in flash to steal, because the private half is reconstructed inside the
  authenticator from a key handle and never leaves it;
* **the key alone cannot approve anything either.** It has never heard of NATS,
  and the derived key means nothing without the `ikm` and the seed key this device
  holds;
* **a `deny` costs a touch too.** The device cannot put its name to a `deny` any
  more than to an `allow` — see §10.18.5, which is the one place this design is
  more awkward than the button next door, and honestly so.

### What changed, and what it cost

The first version of this folder did it the other way round: the device signed
with an Ed25519 key of its own and the security key only granted *permission* to
use it. That design is gone, and the argument that used to be here for it is
worth keeping as the record of what the change bought and what it cost.

| | Then (permission) | Now (the key signs) |
|---|---|---|
| The signature `hook.py` checks | the device's Ed25519 | the authenticator's ECDSA P-256 |
| Where the private key lives | this chip's flash (§10.6's fallback) | inside the security key, never extractable |
| Compromised firmware | holds the signing key, needs a touch | **holds no signing key at all** |
| A lost key | re-enrol, registration survives | re-enrol **and** re-register (§10.18.1) |
| `deny` | free — the device signed it | a second touch (§10.18.5) |
| ARKG on a microcontroller | avoided | ~450 lines in `components/arkg`, host-tested against Python |

The two costs are real and neither is hidden: a re-enrolment invalidates the
registration, and a deny needs a fingertip. What was bought is the row in bold —
there is no longer a private key on this board that anyone could take.

## 10.18.1 Enrolment, and what it now costs

**Enrolment and registration are still two steps, and the order between them is
no longer free.** The key being registered *is* the enrolment's, so:

| | What it tells what | The file | Console |
|---|---|---|---|
| **Enrolment** (§10.18) | gives this device a signing key | `fido.json` | `key enrol` |
| **Registration** (§10.7) | tells the *handler* about that key | `registration.json` | `register <token>` |

**Enrolment comes first**, and `register` refuses with a sentence saying so if
nothing is enrolled. A re-enrolment produces a *different* key, so the allowlist
entry naming the old one is worthless the moment it runs — which is why
`registration.json` records the public key it was made for, why `Registered()`
compares the two on every boot, and why the console prints `registered STALE`
rather than `registered yes` when they differ. §10.10 rule 5 is the reason it is
enforced instead of logged: a device that subscribed with a stale registration
would take requests out of a shared queue group and answer them with signatures
the hook rejects, which is worse than not answering because it is invisible.

`key enrol` says all of this at the moment it happens, in three lines, and so
does the boot log.

### The key has to advertise it first

**`getInfo` is asked before the touch is spent**, and a key whose `extensions` list
does not contain `previewSign` is refused there. This is not politeness: the
signing key is *derived* from that extension, so a key without it has nothing to
derive from and no change on this side can help.

It is also the difference between a diagnosis and an afternoon. A key that does not
support the extension does not say so when asked for a credential — it fails the
`makeCredential` with a CTAP status that names no cause, which from the desk is
indistinguishable from a broken cable, a wrong interface or a firmware bug. The
`getInfo` costs nothing and no touch, so the refusal is free and it names the
reason. `key info` prints the same line at any time.

A key that will not answer `getInfo` **at all** is not refused here — that is a
cable problem and not a wrong key, so it is left to the enrolment to report.

### One `makeCredential`, one touch, two keys

Enrolment is a single `authenticatorMakeCredential` carrying
`previewSign.generateKey`, and what comes back is **two** keys:

* **the credential** — an ordinary ES256 one. It is what proves a human touched
  *this* key for *this* request: its public key verifies the assertion's own
  signature, and the user-presence flag lives inside those signed bytes;
* **the ARKG seed key**, in the response's *unsigned extension outputs*, wrapped
  in a nested attestation object whose `credentialId` is the key handle. That
  seed key is what the device derives its signing key from — offline, on the chip,
  with no further touch.

What is stored (`fido.json`, format 2):

```json
{
    "v": 2,
    "credentialId": "<base64>",
    "publicKey": "<base64 of 04 || X || Y>",
    "userId": "<base64, 16 bytes>",
    "keyHandle": "<base64>",
    "seedBlinding": "<base64 of 04 || X || Y>",
    "seedKem": "<base64 of 04 || X || Y>",
    "ikm": "<base64, 32 bytes>",
    "ctx": "ai-remote-approvals",
    "derivedPublicKey": "<base64, 33 bytes compressed>",
    "aaguid": "<32 hex characters>",
    "vendorId": 4176,
    "productId": 1031
}
```

**The file stores the inputs and the answer, and the answer is re-computed at
every boot.** That is not belt-and-braces: ARKG's own key handle — the KEM
ciphertext the authenticator needs back — is *not* in the file, so loading the
enrolment means running the derivation anyway. Having run it, comparing the
result with `derivedPublicKey` is free, and it catches the one failure that would
otherwise be silent: a file somebody edited, or a derivation that changed
underneath an existing enrolment. A mismatch is refused at boot with a sentence
naming both halves, not discovered one rejected approval at a time.

**`ikm` is in that file and it is not a signing secret.** Anyone holding it plus
the seed public key can derive this device's *public* key — which is already on
the bus in every registration. What they cannot do is produce a signature; that
needs the authenticator. What its leak costs is unlinkability: somebody could tell
that two registrations were the same device.

**The credential is not discoverable** (`rk: false`), so enrolling costs none of a
YubiKey's twenty-five resident slots. The credential id *is* the storage, which is
why losing `fido.json` means re-enrolling, and why `key forget now` says the
credential is still on the key and this firmware has no way to remove it.

The relying party is `approver-esp32-yubikey`, a constant in `ctap2.h`. It matches
`key_id` on purpose, so a key seen in a browser's credential manager names the
thing it belongs to.

**Attestation is not verified**, and that is a decision rather than an omission.
This device has no root store, no way to be told which manufacturers are
acceptable, and no operator to ask — and a check that always passes is worse than
an absent one, because it reads as a check.

### Why there is no PIN

`clientPIN` and `pinUvAuthProtocol` are not implemented and will not be. This
device has one button; there is no way to enter a PIN and no screen to enter it
on. A key that *requires* a PIN cannot be used here, and `key info` says so —
`pin: set — this device cannot enter one; enrolment may refuse` — rather than
letting it be discovered in the middle of an approval.

The enrolment asks for `previewSign` flags `1`, which is "user verification not
required". The draft's other spelling, `0b101`, asks the key to enforce UV on
every future signature — on a device with one button that would be an enrolment
that can never be used again.

What is lost is **user verification**; what is kept is **user presence**, which is
the property this gate is built on. A key enrolled without UV still will not
answer until it is touched.

**And when it is not touched, the key refuses the request rather than timing out.**
A YubiKey answers `CTAP 0x27`, whose spec wording is "operation denied" — which
reads like a device that objected, when what happened is that nobody was there. The
console says so in as many words, because the difference matters: it is a
**nothing**, not a deny (§10.10 rule 2), and it is the failure an operator meets
first.

## 10.18.2 The derivation

`components/arkg` is the instance **ARKG-P256ADD-ECDH**, from
`draft-bradleylundberg-cfrg-arkg` — the one `fido2.cose.ARKG_P256` names:

```
ctx'    = I2OSP(LEN(ctx), 1) || ctx
ctx_bl  = 'ARKG-Derive-Key-BL.'  || ctx'
ctx_kem = 'ARKG-Derive-Key-KEM.' || ctx'
(ikm_tau, kh) = KEM-Encaps(pk_kem, ikm, ctx_kem)
tau = BL-PRF(ikm_tau, ctx_bl)
pk' = pk_bl + tau * G
```

Seven steps, and **five of them are a hash function and some byte strings**:
`expand_message_xmd` (RFC 9380), two HKDF expansions, an HMAC truncated to 128
bits, and a reduction modulo the subgroup order. Two need an elliptic curve: the
ECDH inside the KEM, and the point addition that blinds the key.

So the curve and the hash arrive through a `Backend` of four function pointers,
and the split is the whole testing strategy:

| Step | Where it runs | How it is checked |
|------|---------------|-------------------|
| `expand_message_xmd`, HKDF, HMAC, the reduction, the labels | `arkg.cpp`, includes nothing | §10.11 tier 1, against numbers Python produced — every intermediate, not just the answer |
| ECDH, `scalar * G`, point addition | `arkg_psa.cpp`, PSA Crypto + one mbedTLS call | `key selftest` on the board, against the same vector |

**A wrong derivation is the one failure here with no symptom.** The device would
register a public key whose private half the authenticator cannot reconstruct,
every reply would be rejected by `hook.verify_reply`, and from the desk that is
indistinguishable from a device that is not answering. Nothing logs it. That is
why the vectors are generated by a *second, independent* implementation of the
draft (`tools/make_arkg_vectors.py`, written over `cryptography` alone) and why
`tests/test_esp32yk_arkg_vectors.py` additionally cross-checks the endpoint
against **Yubico's own** `fido2` when the extra is installed. Three
implementations, and the one that decides is the authenticator's.

### The one operation PSA has no name for

`pk' = pk_bl + tau * G` is a point addition. PSA has no entry point for adding two
public points — it is not an operation any protocol PSA was designed for needs.
mbedTLS does: `mbedtls_ecp_muladd`, which computes `m*P + n*Q` and is exactly this
with both scalars at one.

It is reached through `mbedtls/ecp.h`, which on ESP-IDF v6 is **the framework's
own shim** in `components/mbedtls/port/include`: ESP-IDF declares
`MBEDTLS_DECLARE_PRIVATE_IDENTIFIERS` and includes the private header itself. So
this is the supported path to it rather than a reach around one — which matters,
because `fido.cpp` argues in the opposite direction about hashing and the two have
to be consistent. The distinction is that PSA *has* a hash and an ECDSA verify,
and does not have this.

`CONFIG_MBEDTLS_ECP_C` and `CONFIG_MBEDTLS_ECP_DP_SECP256R1_ENABLED` are both on
in the inherited defaults; `build.md` §10.4 has what it costs.

### And the bignum

One, and it is nine lines: reducing a 48-byte integer modulo the subgroup order,
bit by bit, with a single conditional subtraction each. `r < n` holds at the top
of every iteration, so `2r + bit < 2n` and one subtraction restores it — which is
why there is no loop around the subtraction and must never be one. Everything else
in the file is byte strings and calls to the backend.

## 10.18.3 The five checks

`fido::Sign` parses the assertion and then makes five checks. All five have to
pass; failing any of them means **no reply at all** (§10.10 rule 1).

1. **The relying party hash matches.** A key will not normally answer for another
   one, but the hash is inside the signed bytes and checking it costs a `memcmp`.
2. **The user-presence flag is set.** *This is the bit the whole design rests on.*
   A key that answered without presence answered by itself, and an approval nobody
   made is the outcome §10.10 exists to prevent. The extension does not replace
   this: a verdict signature is worth nothing without the flag inside the bytes
   the key signed.
3. **The credential is the one this device enrolled** — when the key bothered to
   say which. With a one-entry allow list many keys omit the descriptor, so an
   absent one is not a failure; a *different* one is, and it is reported as
   `wrong-key` rather than as a bad signature.
4. **The assertion's own signature verifies** over `authData || digest`, against
   the credential's public key. Everything above this line is shape; this line is
   what makes the presence flag evidence rather than a claim.
5. **The verdict's signature verifies** over the digest, against the *derived*
   public key — the one §6 registered.

Check 5 fails in two ways and they are different facts about the key. **Absent**
means the key ignored the extension, which is almost always a key without
`previewSign`, and it is `no-signature`. **Present and wrong** means the derived
key this device registered is not the one the authenticator reconstructed, and it
is `bad-signature` — the loudest outcome this firmware has.

**Catching that here rather than letting the hook catch it is the point.** The
device could publish and let `hook.verify_reply` do the checking; what that would
cost is the difference between one log line naming the problem and an approval
that silently never lands.

**None of the failures is a `deny`.** A gate that timed out, was cancelled, or
returned something that did not verify produces *no reply at all*. A key that
answers with something that does not check out is either the wrong key, a bug in
this firmware, or something between the two pretending to be a key, and none of
those is a reason to publish anything.

### The two crypto libraries, and why there are still two

| | Library | Why it has to be that one |
|---|---|---|
| ECDSA P-256 — verifying both signatures, and the whole derivation | **PSA Crypto** (+ one `mbedtls_ecp_muladd`) | already linked for `esp-tls`, and it is the API v6 means to be called |
| SHA-256 — the digest, the challenge, the derivation's hash | **PSA Crypto** | same |
| Ed25519 — verifying §6's *reply* | **libsodium** | mbedTLS has no EdDSA at all, and §6's server key is Ed25519 by fixed protocol |

**libsodium no longer signs anything on this device.** It is there to check the
registration handler's signature, which is the one place Ed25519 survives here.
§10.6 has what that leaves of `components/crypto` and what is owed.

One consequence worth knowing: `psa_verify_hash` takes ECDSA signatures as raw
`r || s` (64 bytes, each half fixed-width) while CTAP2 hands back DER. The
converter is `ParseDerSignature` in `fido.cpp`, and the leading-zero rule is the
part that bites — DER writes `02 21 00 ff…` for an integer whose top bit is set,
and a converter that copied the length verbatim would produce a 33-byte half that
PSA rejects.

## 10.18.4 The cable

`components/fido` is four files in three layers, and the split is the layering of
§10.14.2:

| File | Knows about | Host-tested? |
|------|-------------|--------------|
| `ctaphid_frames.cpp` | packets, sequence numbers, lengths | **yes**, every branch |
| `cbor.cpp` | RFC 8949, as much of it as CTAP2 needs | **yes**, including the hostile shapes |
| `ctap2.cpp` | requests and responses, `authData`, COSE keys, `previewSign` | **yes**, against hand-built bytes *and* against two synthetic responses the vector generator builds |
| `fido_usb.cpp` | the USB Host Library, enumeration, transfers | **no** — this is the device tier |

The three pure layers are carved out for a reason worth stating: the framing has a
**sequence number**, a **length that arrives before the data**, and a **buffer
whose fill rate is controlled by a device on the other end of a cable**. That is
three of the four ways this component could be made to overrun something, and a
*real* key will never send a malformed frame — which is precisely why the
malformed paths need a test rather than a soak.

**The `previewSign` shapes are generated rather than typed**, and that is new.
Nobody here has seen one on hardware, so writing the expected bytes by hand in a
C++ test would pin one reading of the draft twice. `make_arkg_vectors.py` builds a
`makeCredential` response with the generated key in its unsigned extension
outputs, and an assertion with the signature inside `authData`'s extensions; the
parsers are tested against those. When a real key finally answers, those bytes are
the first thing to compare it with.

### Picking the right interface

A YubiKey 5 presents **three** USB interfaces: an HID keyboard (the OTP slot), the
FIDO HID interface, and CCID. Picking the wrong one means writing CTAPHID frames
at a keyboard.

The discriminator here is the **endpoint shape** — class HID, an interrupt IN
*and* an interrupt OUT, both at least 64 bytes — rather than the HID report
descriptor's usage page (`0xF1D0`), which is the formally correct answer and needs
a control transfer plus a second descriptor format to parse. The shape is
unambiguous on every FIDO key, because a keyboard interface has no OUT endpoint
and no 64-byte reports.

And the guess is **proved rather than assumed**: the first thing this driver does
with a claimed interface is `CTAPHID_INIT`, and an interface that is not CTAPHID
does not answer one — with a nonce echo check, so a reply on the shared broadcast
channel that is not ours is not mistaken for one. A wrong guess costs a log line,
not a wedged key.

### What it costs, and the one rule it bends

§10.14.1 forbids dynamic memory. The USB Host Library allocates its transfer
buffers through `usb_host_transfer_alloc` and there is no static form of it. That
is **two 64-byte transfers, allocated when a key is plugged in and freed when it is
unplugged** — a one-shot path at a device boundary, which is the same allowance
`config.cpp` takes for cJSON and states in the same words. It is not a per-exchange
allocation and it must never become one.

### Who owns a transfer, and the two bugs that came of not knowing

**The Host Library's rules about a `usb_transfer_t` are all of the form "not while
the driver has it"**, and this file used to track no such thing. A queued transfer
may not be submitted again — `usb_host_transfer_submit` answers
`ESP_ERR_NOT_FINISHED` — may not be read out of, and may not be freed:
`usb_host_transfer_free`'s own header says so in one line, because the host
controller writes into that buffer when the transfer finally completes.

Two things followed, and both were real.

**1. A read whose wait ran out abandoned its transfer.** `ReadPacket` submitted a
fresh IN transfer on every call and simply returned when its wait expired, leaving
the previous one queued. Every submit on that endpoint afterwards was refused, so
the read loop had nothing left to block on — and a read loop that cannot block is a
busy loop, spinning for whatever remained of the exchange's budget. This was
already written down in the code as the reason the 300 ms read wait could not be
shortened: tried at 10 ms, it became `the key could not be reached` sixty
milliseconds into every exchange.

**2. `CloseDevice` freed transfers the controller still owned** — and the path that
reaches it most is the one this component exists to survive, a key pulled out of
the port mid-read. It freed both buffers, then released the interface and closed
the device without looking at either return value, though neither can succeed while
a pipe still has URBs queued on it.

**The fix is that a transfer is ended through its endpoint, not through itself.**
`Reclaim` halts the pipe, flushes it — which cancels what is queued — and clears
the halt; the cancellation then arrives as an ordinary completion with
`USB_TRANSFER_STATUS_CANCELED`, and `Reclaim` waits for it before calling the
transfer ours again. Two details are load-bearing:

* **a flush can race a real completion**, so `Reclaim` reports only whether the
  transfer is *safe to touch* and leaves the status to the caller. A key that
  answered in the moment between the wait expiring and the halt going out has
  handed us a genuine packet, and discarding it would lose a keepalive — or an
  assertion somebody had just touched the key for;
* **`CloseDevice` runs on the client task, which is the task that delivers
  completion callbacks**, so it cannot wait on a semaphore for one: that is waiting
  for itself. It pumps its own event queue instead, and a transfer that still does
  not come back is **kept rather than freed** — the pointers stay and the next
  device re-uses them, because handing a buffer back while the controller owns it
  is the one outcome worse than holding on to it.

A read now reports **three** outcomes rather than two, and the third is the point:
"nothing arrived, ask again" and "this endpoint is unusable" used to be the same
`false`. `Read::kBroken` ends the exchange instead of asking again immediately.

Three counters in `key` say what is happening: `reclaimed` is the ordinary cost of
waiting on a key that is thinking and climbs by about one per timed-out exchange;
`stuck` has never been anything but zero and would mean an endpoint is finished
until the key is unplugged; `busy waits` belongs to the section below.

**300 ms is still the read wait, and the reason has changed.** It was a
correctness floor and is now only a cost one: every expiry buys a halt, a flush, a
clear and a task handoff on a pipe that was working. Whatever needs sampling faster
than this is still sampled somewhere else, which is why `buttons` has a poller of
its own.

### `CHANNEL_BUSY`, and waiting out a key that is holding a dead transaction

A key holds **one** transaction at a time, across every channel. Reset the board
while a request is waiting for a fingertip and the key goes on waiting for a
channel that died with the previous boot — and there is nothing left to send
`CTAPHID_CANCEL` to, because the channel identifier that owned the transaction went
with it. Everything after that answers `CTAPHID_ERROR 0x06`.

**It cannot be reset from firmware.** `usb_host.h` at this version has neither a
port reset nor a device reset, and VBUS on this board's OTG socket is not
switchable (§10.1), so there is no way to take the key's power away either.

**And it does not need to be**, because the stale transaction is a user-presence
wait and it expires: measured on the key on this desk at **about 34 seconds from
when the stale request started** — busy at 8.4 s into a run, free at 42.9 s. So
`Exchange` waits it out, asking again every 500 ms, **inside the budget the caller
already gave it and never beyond it** — the caller's budget is a request's
deadline, and §10.10 rule 1 says silence is the safe end of one. The wait stays
cancellable throughout: the caller's keep-alive hook is offered at roughly the
keepalive cadence, so a tap on BOOT or a request that ran out on the bus still
stops it.

`CTAPHID_INIT` is inside the loop rather than in front of it, because a busy key
refuses that too.

**This loop was written once before and reverted**: it recovered correctly and
rebooted the board about one run in two, roughly 1.3 s after the first `0x06`. The
suspect named at the time was the transfer lifecycle above — every attempt
re-submitted an IN transfer the controller still owned — and that is what
`Reclaim` was for. With it in place the same repro has been run five times with no
reboot at all: 21 to 50 retry rounds per run, `0 stuck`, `0 transfer` errors, and
the key freeing itself on schedule.

Three endings, and the third is the one worth being careful about: a stale
transaction that expires hands the request straight on to the key, which then waits
for a fingertip like any other. So an exchange that recovered and *then* timed out
on the finger is **not** a key that was still busy, and the log says which — saying
otherwise would send somebody to unplug a key that had already fixed itself.

### The wedge this device was inflicting on itself

Found by the retry loop, on the run that proved it. **An exchange that ran out of
its own deadline walked away without telling the key to forget the request** — so
the abandoned request went on holding the key's one transaction slot for the rest
of its user-presence window, and everything after it answered `0x06`. The same
wedge as a reset, except self-inflicted, and unlike the reset case entirely
avoidable: the channel is still ours, so there is somewhere to send the cancel.

`Transact` now cancels and drains on its deadline, the way it already did when the
keep-alive hook gave up. The cost is up to `kDrainMs` past a deadline that has
already failed; the alternative was a key that answered nothing for half a minute.
On the board: a `key test` nobody touches is now followed by a `key info` that
works, where before it was followed by a `key info` that spent its whole budget
retrying against a key this device had wedged.

### Waiting

**A request arriving with nothing in the OTG port is the ordinary case**, not an
edge one: the operator sees the light, reaches into a pocket, and plugs one in. So
the gate *waits* for a key rather than refusing at the door — polling the button,
the bus and the deadline at 100 ms until a key appears, somebody taps BOOT, the
bus goes, or the request runs out. The deadline is a monotonic one off
`esp_timer`, which is the only kind this board has (§10.13).

That is not only a convenience. The first version refused immediately, which
collapsed the whole gate to a microsecond: nothing was decided, the request stayed
at the head of the queue, and the gate task took it again — several hundred times
a second, each with a log line, until the TTL ran out. **A fail-safe that fails
loudly enough to drown the console is not a fail-safe.** `g_abandoned_nonce` in
`responder.cpp` is the second half of that fix: a request the gate has given up on
is remembered and not re-gated, and expires on the queue where it belongs.

While the key is holding an exchange open waiting for a fingertip it sends
`CTAPHID_KEEPALIVE` roughly twice a second, and the hook that receives it is where
the three early exits live: a tap on BOOT (a deny), a bus that went round the
houses, and the request's own deadline. Returning false from it sends
`CTAPHID_CANCEL` — so a request that timed out on the bus stops asking the
operator for a fingertip nobody is waiting for.

## 10.18.5 A deny costs a touch

The BOOT button chooses `deny`. **The key signs it**, because there is nothing
else on this device that can sign anything.

So a deny is two gestures: tap BOOT, then touch the key. It is more awkward than
the sibling board's single press and it is the honest consequence of the private
key living where it does.

**What happens in between took three fixes to get right**, and all three were
invisible until a real deny was tried on the board:

* **the tap has to be seen.** The gate is blocked inside a USB read for the whole
  of a request and is consulted only when the key speaks — every 100 to 300 ms —
  while `buttons`' debounce promotes a level that has held *across* two polls. BOOT
  therefore had to be held for most of a second, and an ordinary tap disappeared
  between two samples. `buttons` owns a 10 ms poller now and latches the press
  until the gate collects it; `buttons.h` carries the argument;
* **the light has to change.** It did not: the tap altered nothing an operator
  could see, and the next thing anybody does is touch the key — which signs an
  `allow`. Twice on the desk that is exactly what happened. `deny-pending` is red
  and lasts as long as the wait (§10.17);
* **the cancelled request has to be cleared.** The tap cancels the key's request
  for an `allow` with `CTAPHID_CANCEL`, and the key still answers the request it
  abandoned — `CTAP2_ERR_KEEPALIVE_CANCEL`, `0x2D`. Left in the pipe, that answer
  is what the deny's `getAssertion` read as its own: fourteen milliseconds,
  `Gate::kCancelled`, and a red light that existed for seventeen. `fido_usb.cpp`
  drains it now.

So the sequence an operator actually sees is: **white and the key blinking → tap
BOOT → the key stops asking → red and the key blinking again → touch.** The console
says `denied - touch the key to sign it` at the tap.

**Walking away is not a failure to be smoothed over — it is the third outcome,
and the safe one.** No touch, no reply, the hook times out, and Claude Code asks
in its own terminal (§10.10 rule 1). The counters say which happened:
`button_denied` counts the taps, `gate_declined` counts the times nothing was
signed afterwards, and §10.10 rule 2's distinction between a deny and a silence
survives intact.

While the deny is being signed the button is **disarmed** — a finger still resting
on BOOT from the tap that chose `deny` must not cancel the exchange half a
millisecond later.

**And a deny tapped with nothing in the OTG port is refused at once**, not waited
on. The gate already waited for a key before it asked for the `allow`; if the tap
came during that wait, there is no key to sign with and the outcome is the third
one — no reply, the request abandoned, the hook's own timeout. That is the same
answer walking away would have given, arrived at sooner.

## 10.18.6 There is no development mode

`approval.requireKey` is gone, and its absence is worth a section because the
setting existed for two versions of this document.

It switched the security key *off*: the BOOT button alone then approved, with a
two-second hold, and every readout on the device shouted about it. That was
possible because the device had a signing key of its own. It does not any more —
the private half lives inside the authenticator — so there is no mode in which a
button alone approves, and there is nothing for a setting to switch.

`config set requirekey` therefore answers with a sentence saying so rather than
"unknown setting": an operator typing it is holding an old instruction, not making
a typo. What remains in `config.json` is:

```json
"approval": { "touchTimeoutSeconds": 30, "denyButton": true }
```

`touchTimeoutSeconds` is shorter than the hook's own timeout on purpose: a light
still asking after the asker has stopped listening is a light that means nothing.
`denyButton` decides whether BOOT can choose a `deny` at all.
