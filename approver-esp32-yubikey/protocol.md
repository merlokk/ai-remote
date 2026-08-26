# The bus, the key and the registration (§10.5, §10.6, §10.7)

**Most of this is the sibling board's, unchanged, and this document says which
parts and why rather than copying them.**
[`../approver-esp32/protocol.md`](../approver-esp32/protocol.md) is the long form
of §10.5, §10.6 and §10.7, and it is authoritative for everything not
contradicted below. That is not laziness: `components/nats` and `components/crypto`
are **the same code**, down to a pair of adapted header comments, and two copies of
a rationale is one copy that drifts.

**§10.6 is the exception and it is now a large one.** Since §10.18 this device does
not sign with a key of its own at all, so `components/protocol` and
`components/registration` have diverged — one constant and one ordering rule — and
key custody has stopped being a question this board answers.

What follows is what this device does differently, and the one thing that is
identical for a reason worth restating.

## 10.5 The bus

Identical. `components/nats` is the same four files: an endpoint parser, a link
policy, the `debsahu/espidf-nats` client and the wrapper over it. Same subjects,
same queue group, same 64 KB ceiling, same reconnect-invalidates-subscriptions
behaviour.

**One difference, and it is a subtraction: this device subscribes to exactly one
subject.** `approvals.*` is the only thing it has any use for — there is no display
here, so a subscription whose only purpose would be a readout would be deliveries
taken off the bus task to fill a struct nobody reads.

What is on `approvals.*` is unchanged, and that is the point.

## 10.6 Key custody: **not this board's problem any more**

This is the section that stopped being the same as next door.

The sibling board signs verdicts with an Ed25519 key derived from an eFuse
secret — or, where no eFuse key is burned, from **a seed in unencrypted NVS**,
which its own §10.6 calls "strictly worse" and which it is: `esptool read_flash`
gives up the signing key.

**This device has no signing key of its own** (§10.18). What it registers is an
ARKG-derived P-256 public key; the private half is reconstructed inside the
security key from a key handle, per signature, and never exists on this chip. So
the whole eFuse-versus-NVS question is not answered here — it is **absent**:

| | The C6 board | This one |
|---|---|---|
| What signs a verdict | an Ed25519 key on the board | the authenticator's ECDSA P-256 |
| Where the private key rests | eFuse-derived, or a seed in flash | inside the security key |
| A flash dump yields | the signing key, in the fallback | `ikm`, a seed *public* key and a credential id — none of which can sign. **No private key of any kind**, since the Ed25519 seed was deleted |
| Losing the board | the key is compromised | nothing is; the key walks away in a pocket |
| Losing the key | — | this device cannot answer until it re-enrols and re-registers |

**What is in `fido.json` is worth being precise about.** It holds `ikm`, the seed
public key, a credential id and a key handle. Anyone with all of it can derive this
device's *public* key — which is on the bus in every registration anyway — and can
tell that two registrations were the same device. None of it produces a signature.
§10.18.1 says the same thing where the file is defined.

### What Ed25519 is still doing here, and what stopped

**The identity is gone.** `components/crypto` used to derive one at boot — an eFuse
route through the HMAC unit, and a stored-seed fallback in NVS — and it signed
nothing after §10.18 moved the signer into the security key. It has been deleted:
the seed, the eFuse route, `Sign`, `ProveKey`, `keys forget now`, and the constant
that sized every stack that might have signed. The files went with it —
`device_key.h` and `device_key.cpp` are `crypto.h` and `crypto.cpp`, because a file
named for a device key in a firmware that has none is a file that misleads.

**The removal is the point, not the tidiness.** §10.6 called the stored seed
"strictly worse" than an eFuse and it was: NVS is as readable as SPIFFS until flash
encryption is burned, so `esptool read_flash` gave up a signing key. It now gives up
a Wi-Fi passphrase and nothing else. **`crypto::Init` also erases the seed a
previous firmware stored** — deleting the code that read it would otherwise have
left the bytes exactly where they were, a private key belonging to nothing that
anybody with the board could still read. It says so once, and it has:

```
W crypto: erased the 32-byte Ed25519 seed an older firmware stored - this board
          now holds no private key at all (§10.6)
```

Two things keep the component, and neither ever needed a key of our own:

* **verifying the handler's reply.** §6's server key is Ed25519 by fixed protocol
  and mbedTLS has no EdDSA at all, so libsodium stays. `crypto::Verify` is static:
  it takes the public key it is checking and holds nothing. This is now the *only*
  cryptographic thing in the component, and a fresh registration on the board is
  what proves it still works;
* **base64**, which half this firmware uses.

**The boot self-test still runs, and it is now a verify.**
`CONFIG_LIBSODIUM_USE_MBEDTLS_SHA` is a seam that has historically produced
valid-looking but *wrong* Ed25519 output (esp-idf#1044), and a broken SHA breaks
checking a signature exactly as thoroughly as making one. So `keys selftest` does
two things against a vector generated by `lib/crypto.py`: a real signature must
verify, **and a one-bit-flipped copy of it must not** — because a verifier that says
yes to everything passes the first check on its own. It signs nothing, so the
vector's seed is now unused.

What follows from a failure is stronger than it was. `crypto::Ready()` no longer
means "this board has a key"; it means "the handler's signature can be checked", and
`responder::Blocker::kCannotVerify` keeps the device off `approvals.*` without it. A
board that cannot verify §6's reply cannot know **whose** key it pinned, and pinning
the wrong one is worse than not registering at all.

**And there is a second self-test now, for the half that matters more.** `key
selftest` runs the ARKG derivation's two curve operations — the ECDH and the point
addition — against a vector compiled into the firmware, on this silicon, with
nothing plugged into the OTG port. A failure there means no security key would ever
have worked (§10.18.2).

## 10.7 Registration, and the console

Identical mechanics, one different `key_id`, and one new blocker.

The exchange: publish `{v, token, key_id, pubkey, key_type, nonce, ts}` on
`registrations`, wait on a private inbox, **verify the handler's Ed25519 signature
before reading a single field of the reply**, and only on a verified `ok:true`
write `key_id` and the pinned handler key to `registration.json`. The ordering is
enforced in `protocol::ParseRegistrationReply`, which takes the verifier as an
argument so there is no way to get the fields out without it having run.

`key_id` is **`approver-esp32-yubikey`**, and it is a constant in
`protocol/registration.h` rather than a setting. It is half of what an allowlist
entry is bound to, and an operator who could change it could make this device
answer as another one. `key_type` is a constant there too, and it is now
**`p256`**: the key being registered is the ARKG-derived one (§10.18), and nothing
this device sends may choose an algorithm.

**And `pubkey` comes from the enrolment**, which is the ordering that changed:
`register` refuses, with a sentence saying so, if nothing is enrolled. There is no
weaker key to fall back on — there is no key at all.

### What it looks like when it works

```
> register approver-esp32-yubikey./2xVlAhNOQrTQKu9iBeLr69tx4QLftcGYjqpRqAqhAE=
registering as approver-esp32-yubikey...
registered as approver-esp32-yubikey, handler key Q15MkgcK2zYbfbLfxF2CFN8jyRRluEjWxbHnhRL1Zv0=

check that handler key against what the handler printed at startup. it is
pinned now: a reply signed by any other key will be refused from here on.
```

Trust on first use, and the console is what closes it: with nothing pinned the
handler's key is taken on trust and pinned for every registration afterwards; with
something pinned, a reply signed by any other key is refused rather than
re-pinned. The line above is there so an operator can compare it, once, by eye
with what the handler printed.

**The nonce is generated after the radio is up.** The ESP32's RNG is only a true
random source with the radio enabled, and `Register` refuses without a bus
connection — which means a client link, which means the radio. A predictable nonce
gives away exactly the replay protection the nonce exists for.

### Registration, and the enrolment underneath it

**A registration names one public key, and on this device that key belongs to the
enrolment.** So a `key enrol` invalidates it: the handler's allowlist entry now
describes a key nothing holds, and every reply this device signed would be rejected.

`registration.json` therefore records the `pubkey` it was made with, and
`registration::Registered()` compares it with the enrolment's on every boot and
every tick. When they differ the device is **not registered** as far as the
responder is concerned — it stays off `approvals.*` — and the console says which:

```
> request
key        A6t4…    (the key enrolled now)
registered STALE - for AmNq…, not the key enrolled now
```

The fix is a fresh token and `register` again. There is no way to re-point an
allowlist entry from the device, and there should not be: the token is what proves
somebody with access to the handler agreed to this key.

### The blocker that is new here

The sibling board's responder subscribes when it has a key, a registration and a
connection. This one needs the enrolment, because on this device **the enrolment
is the key**.

That was added after this device's first real registration, when it did exactly
what the rule forbids — went straight onto `approvals.*` with nothing enrolled and
started taking requests out of the `approvers` queue group that it could not
answer. §6's queue group means each request reaches exactly *one* responder, so
every request it swallowed was a request the YubiKey responder never saw,
answered with silence, and looking from the outside exactly like a bus that had
gone quiet.

`responder::Blocker::kNotEnrolled` is where that is enforced, and `request` on the
console names it:

```
> request
responder  no security key enrolled - run 'key enrol'
```

**Enrolment blocks; a key not being plugged in does not.** They are different
kinds of fact: an enrolment is permanent and its absence means *never*, while a
key in a pocket is a fifteen-second problem the gate already waits out. Blocking
on presence would take this device off the subject every time the operator walked
away with the key.

### The console

Same `esp_console` REPL, same command style, **a different port**: UART0 through
the CH343P bridge, because the chip's own USB is a host for the security key
(§10.1, §10.18.4). `commands.md` is the list of what you can type.

The up-arrow is off until you type `term`, and the reason is inherited from the
sibling board: the probe that would enable line editing runs while the REPL is
created, before anybody is attached to answer it. **That reason may well not apply
here** — the bridge is a real UART that exists from power-on, so the probe may
simply be answered — and `console.cpp` says so at the line rather than pretending
the question is settled. Nobody has measured it on this board; §10.11's device
tier is where that gets checked.
