# The key (§10.18)

**What has to happen between an operator deciding and this device signing.**

## 10.18 What the key is, and what it is not

**The key does not produce the verdict's signature.** That is the device's own
Ed25519 key (§10.6) — the one `registration_handler.py` has in its allowlist and
the one `hook.verify_reply` checks. Nothing on the host side changes for this
device, which is the point: `hook.py`, `protocol.py` and
`registration_handler.py` must not know it exists.

**What the key produces is permission to use that signature.** Before a byte is
signed, `fido::RequireTouch` asks a FIDO authenticator for an assertion over *the
exact bytes about to be signed*, and the key will not answer until somebody
touches it.

The challenge is `SHA-256(protocol::DecisionSigningBytes)` for **`behavior:
"allow"`**. Four things follow, and each of them is the reason for a design choice
elsewhere:

* **a touch cannot be replayed onto a different request.** The bytes carry the
  session, the nonce, the tool, the input hash and the timestamp; a different
  request is a different challenge and needs a different touch;
* **the key can only ever authorise an `allow`.** The bytes it commits to say
  `allow`. There is no way to manufacture a `deny` out of an assertion, which is
  why the deny path does not go anywhere near the key;
* **the device alone cannot approve anything.** Firmware that had been tampered
  with, holding the Ed25519 seed, still needs the physical key present and a
  finger on it;
* **the key alone cannot approve anything either.** It has never heard of NATS,
  and the credential it holds is meaningless without the device that enrolled it.

### Why not sign the verdict with the key itself

`approver/responder_yubikey.py` does exactly that, over §8's ARKG `previewSign`
flow, and it produces a P-256 signature `crypto.py` already verifies. It was the
obvious design and it is not this one, for three reasons in descending order of
weight:

1. **ARKG on a microcontroller is a different project.** The derivation, the
   blinding, the credential management and the PIN/UV auth protocol underneath it
   are what `lib/yubikey.py` needs `fido2` for, and none of it exists in C;
2. **the registration would have to change.** §6 registers one public key per
   `key_id`; an ARKG responder registers a *derived* key and re-derives per
   request. Doing that from the device means the handler learns about a second
   shape of registration, and root §2's rule is that the four responders look the
   same to it;
3. **it buys nothing this design does not already have.** The property being
   bought is "a human was physically present and authorised *this* request", and
   an assertion over the request's own bytes establishes exactly that. Where the
   final signature comes from is a question about key custody (§10.6), not about
   presence.

### Why there is no PIN

`clientPIN` and `pinUvAuthProtocol` are not implemented and will not be. This
device has one button; there is no way to enter a PIN and no screen to enter it
on. A key that *requires* a PIN cannot be used here, and `key info` says so —
`pin: set — this device cannot enter one; enrolment may refuse` — rather than
letting it be discovered in the middle of an approval.

What is lost is **user verification**; what is kept is **user presence**, which
is the property this gate is actually built on. A key enrolled without UV still
will not answer until it is touched.

## 10.18.1 Enrolment is not registration

Two steps that both happen when the device is first set up, and they are
deliberately separate:

| | What it tells what | The file | Console |
|---|---|---|---|
| **Registration** (§10.7) | tells the *handler* about this device | `registration.json` | `register <token>` |
| **Enrolment** (§10.18) | tells this *device* about a key | `fido.json` | `key enrol` |

Keeping them apart means a re-registration does not cost the key enrolment, and
re-enrolling on a spare key does not cost the registration. Three lifetimes,
three files — the same argument §10.15 makes for keeping `config.json` and
`registration.json` apart.

Enrolment is one `authenticatorMakeCredential` and one touch. What comes back and
is stored:

```json
{
    "credentialId": "<base64>",
    "publicKey": "<base64 of 04 || X || Y>",
    "userId": "<base64, 16 bytes>",
    "aaguid": "<32 hex characters>",
    "vendorId": 4176,
    "productId": 1031
}
```

**The credential is not discoverable** (`rk: false`), so enrolling this device
costs none of a YubiKey's twenty-five resident slots. The credential id *is* the
storage, and it lives in that file — which is why losing `fido.json` means
re-enrolling, and why `key forget now` says the credential is still on the key and
this firmware has no way to remove it.

The relying party is `approver-esp32-yubikey`, a constant in `ctap2.h`. It matches
`key_id` on purpose, so a key seen in a browser's credential manager names the
thing it belongs to.

**Attestation is not verified**, and that is a decision rather than an omission.
This device has no root store, no way to be told which manufacturers are
acceptable, and no operator to ask — and a check that always passes is worse than
an absent one, because it reads as a check. What the enrolment binds is *this
credential to this device*, which is `authData`'s job.

## 10.18.2 The four checks

`fido::RequireTouch` parses the assertion and then makes four checks. All four
have to pass; failing any of them lands on `kBadSignature`, which is deliberately
the loudest outcome this firmware has.

1. **The relying party hash matches.** A key will not normally answer for another
   one, but the hash is inside the signed bytes and checking it costs a `memcmp`.
2. **The user-presence flag is set.** *This is the bit the whole design rests on.*
   A key that answered without presence answered by itself, and an approval nobody
   made is the outcome §10.10 exists to prevent.
3. **The credential is the one this device enrolled** — when the key bothered to
   say which. With a one-entry allow list many keys omit the descriptor, so an
   absent one is not a failure; a *different* one is, and it is reported as
   `wrong-key` rather than as a bad signature.
4. **The ECDSA signature over `authData || challenge` verifies** against the
   public key stored at enrolment. Everything above this line is shape; this line
   is what makes it evidence.

**None of the failures is a `deny`.** A gate that timed out, was cancelled, or
returned something that did not verify produces *no reply at all* (§10.10 rule 6).
A key that answers with something that does not check out is either the wrong key,
a bug in this firmware, or something between the two pretending to be a key, and
none of those is a reason to sign anything.

### The two crypto libraries, and why there are two

| | Library | Why it has to be that one |
|---|---|---|
| Ed25519 — the verdict's signature, and verifying §6's reply | **libsodium** | mbedTLS has no EdDSA at all, and §6's server key is Ed25519 by fixed protocol |
| ECDSA P-256 — verifying the key's assertion | **mbedTLS, through PSA Crypto** | already linked for `esp-tls`, and it does have ECDSA |
| SHA-256 — the challenge, and the assertion's digest | **PSA Crypto** | see below |

**PSA rather than `mbedtls_sha256` / `mbedtls_ecdsa_verify`, and that is ESP-IDF
v6 talking.** On v6 the mbedTLS in the tree is the TF-PSA-Crypto one, and every
classic entry point has moved into `mbedtls/private/`. Reaching into a directory
called `private` to verify a security key's signature would be the wrong kind of
clever.

One consequence worth knowing: `psa_verify_hash` takes ECDSA signatures as raw
`r || s` (64 bytes, each half fixed-width) while CTAP2 hands back DER. The
converter is `ParseDerSignature` in `fido.cpp`, and the leading-zero rule is the
part that bites — DER writes `02 21 00 ff…` for an integer whose top bit is set,
and a converter that copied the length verbatim would produce a 33-byte half that
PSA rejects.

## 10.18.3 The cable

`components/fido` is four files in three layers, and the split is the layering of
§10.14.2:

| File | Knows about | Host-tested? |
|------|-------------|--------------|
| `ctaphid_frames.cpp` | packets, sequence numbers, lengths | **yes**, every branch |
| `cbor.cpp` | RFC 8949, as much of it as CTAP2 needs | **yes**, including the hostile shapes |
| `ctap2.cpp` | requests and responses, `authData`, COSE keys | **yes**, against hand-built bytes |
| `fido_usb.cpp` | the USB Host Library, enumeration, transfers | **no** — this is the device tier |

The three pure layers are carved out for a reason worth stating: the framing has a
**sequence number**, a **length that arrives before the data**, and a **buffer
whose fill rate is controlled by a device on the other end of a cable**. That is
three of the four ways this component could be made to overrun something, and a
*real* key will never send a malformed frame — which is precisely why the
malformed paths need a test rather than a soak.

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

### Waiting

**A request arriving with nothing in the OTG port is the ordinary case**, not an
edge one: the operator sees the light, reaches into a pocket, and plugs one in. So
the gate *waits* for a key rather than refusing at the door — polling the button,
the bus and the clock at 100 ms until a key appears, somebody taps BOOT, the bus
goes, or the request runs out.

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

## 10.18.4 The development mode

`config.json` has one field that makes this device *less* careful:

```json
"approval": { "requireKey": true, "touchTimeoutSeconds": 30, "denyButton": true }
```

With `requireKey` false, the button alone decides: **a two-second hold is an
`allow`** and a tap is a `deny`. The hold is deliberately awkward — long enough
that nobody produces one by accident, and awkward enough to be an honest signal
that this is not the mode the device is meant to run in.

It says so, loudly, in four places: at boot, on every gate, in the reply to
`config set requirekey off`, and in `config` and `key`'s own readouts (`key
*** NOT REQUIRED ***`). **A device that can approve without a key must never be
one somebody forgot they configured.**

It is also read the strict way: anything that is not a JSON boolean leaves the
default alone. A file that says `"requireKey": "false"` is a file with a mistake
in it, and reading that string as `true`-because-non-empty — or as `false` because
somebody meant it — are both worse than ignoring it, and one of them is worse in
the direction §10.10 exists to prevent.

`touchTimeoutSeconds` is shorter than the hook's own timeout on purpose: a light
still asking after the asker has stopped listening is a light that means nothing.
