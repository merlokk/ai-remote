#pragma once

// **The gate, and the signer** (CLAUDE.md §10.18): what has to happen between an
// operator deciding and a verdict existing.
//
// ## The key is the signer
//
// **The verdict's signature is made inside the security key.** This device holds
// no private key that could produce one: what it holds is a *public* key it
// derived from the authenticator's ARKG seed key, the handle that lets the
// authenticator rebuild the private half, and a credential that proves somebody
// touched the thing.
//
// That is §8's scheme — ARKG over CTAP2's `previewSign` — and it is the same one
// `approver/responder_yubikey.py` uses, deliberately: what §6 registers is a
// `key_type: "p256"` public key and what §7 carries is an ECDSA signature, both of
// which `lib/crypto.py` has verified since §8.7. **Nothing on the host side knows
// this device exists.**
//
// Four properties fall out, and they are the reason this design is worth the USB
// stack and the derivation:
//
//   * **a touch cannot be replayed onto a different request.** What gets signed is
//     `SHA-256(protocol::DecisionSigningBytes)` — the session, the nonce, the
//     tool, the input hash, the verdict and the timestamp. A different request is a
//     different digest and needs a different touch;
//   * **the device alone cannot approve anything, literally.** Tampered firmware
//     has no signing key to abuse: there is no seed in this board's flash;
//   * **the key alone cannot approve anything either.** It has never heard of NATS,
//     and the derived key means nothing without the `ikm` and the seed key here;
//   * **a `deny` costs a touch too**, because this device cannot put its name to
//     one any more than to an `allow` (§10.18.5).
//
// ## Two steps, and the order between them is no longer free
//
// `key enrol` here gives this device a signing key: one
// `authenticatorMakeCredential` carrying `previewSign.generateKey`, one touch, and
// what comes back — a credential, an ARKG seed key and a key handle — goes in
// `fido.json` along with the key derived from it.
//
// §10.7's `register` then tells the *handler* about that key. **Enrolment comes
// first**, and a re-enrolment invalidates the registration: the derived key is new,
// so the allowlist entry naming the old one is worthless (§10.18.1).
//
// **The credential is not discoverable** (`rk: false`), so enrolling this device
// uses none of a YubiKey's resident slots. The credential id *is* the storage, and
// it lives here.
//
// ## And the rule it inherits
//
// §10.10: no answer is the safe outcome. Every failure below — no key, no
// enrolment, a touch that never came, a signature that will not verify — ends in
// `Sign` returning something other than `kApproved`, the responder publishing
// nothing, and the hook falling back to its own terminal.

#include <cstddef>
#include <cstdint>

#include "arkg.h"
#include "ctap2.h"
#include "esp_err.h"
#include "fido_usb.h"

namespace fido {

// The enrolment file (§10.15's neighbourhood, and its own file for the reason
// `registration.json` is its own: three lifetimes, three files). A `config
// restore` does not touch it and neither does a re-registration.
inline constexpr const char *kPath = "fido.json";
inline constexpr const char *kTempPath = "fido.json.new";

// Longer than the JSON this writes with every field at its ceiling: a 256-byte
// credential id and a 256-byte key handle are 344 base64 characters each, and there
// are five more blobs besides.
inline constexpr size_t kMaxFileSize = 2048;

// The format the file carries. **2 is the ARKG one** — a v1 file holds a credential
// and no seed key, so it cannot produce a signing key at all and is treated as *no
// enrolment* rather than migrated. Nothing was ever written by a real enrolment
// (`status.md`), so there is no v1 file in the world to be gentle with.
inline constexpr int kFormatVersion = 2;

// What was learned at enrolment and is needed at every assertion afterwards. Two
// halves, and the split is §10.18's:
//
//   * **the credential** — what proves a human touched *this* key for *this*
//     request. Its public key verifies the assertion's own signature, and the
//     user-presence flag inside those signed bytes is the evidence;
//   * **the ARKG seed key and the derivation** — what makes the verdict's
//     signature. The public half is what §6 registered; the private half exists
//     only inside the authenticator, and only while it is holding the key handle.
//
// Neither half is any use without the other, which is the property this device is
// built on.
struct Enrolment {
    bool present = false;

    uint8_t user_id[ctap2::kUserIdSize] = {};

    uint8_t credential_id[ctap2::kMaxCredentialIdSize] = {};
    size_t credential_id_length = 0;

    // The credential's own public key, uncompressed `04 || X || Y`. **This is what
    // makes an assertion evidence**: without it, an assertion could only be checked
    // for being well-formed, and a key that merely answers is not a key that is
    // yours.
    uint8_t public_key[ctap2::kP256PointSize] = {};

    // `previewSign`'s generated key: the handle the authenticator needs back before
    // it will reconstruct a derived private half, and the seed key's two points.
    uint8_t key_handle[ctap2::kMaxKeyHandleSize] = {};
    size_t key_handle_length = 0;
    arkg::SeedKey seed;

    // The entropy and the label this device's key was derived with. Kept rather
    // than assumed: the derivation is re-run at every boot and checked against
    // `derived`, so a file somebody edited fails at load instead of producing
    // signatures nothing can verify.
    uint8_t ikm[arkg::kIkmMin] = {};
    char ctx[arkg::kCtxMax + 1] = {};

    // The answer. `derived.compressed` in base64 is what `registration_handler.py`
    // has in its allowlist and what `hook.verify_reply` looks up (§6, §10.2).
    arkg::Derived derived;

    // Which key it was enrolled on, for a console line and for the one message
    // worth getting right — "this is not the key this device was set up with"
    // reads very differently from "this key said no".
    uint16_t vendor_id = 0;
    uint16_t product_id = 0;
    char aaguid[33] = {};  // 16 bytes, hex, NUL-terminated
};

// What the gate decided. **Everything except `kApproved` means nothing gets
// signed** (§10.10) — the enumerators exist so the operator can be told which
// kind of nothing it was, not so a caller can treat some of them as yes.
enum class Gate : uint8_t {
    kApproved = 0,
    kNoKey,          // nothing on the OTG port
    kNotEnrolled,    // a key, and this device has never been introduced to one
    kWrongKey,       // a key that does not hold this device's credential
    kDeclined,       // the key refused — a PIN it wants, an option it lacks
    kTimeout,        // nobody touched it
    kCancelled,      // the request went away while we were asking
    kBadSignature,   // **the loud one** — see the note in `fido.cpp`
    kNoSignature,    // it answered, and the extension's signature was not there
    kTransport,      // the cable, in one of `usb::Fault`'s ways
};

const char *GateName(Gate gate);
const char *GateText(Gate gate);

// Loads `fido.json` if there is one and brings up the transport. A missing file
// is not an error: it is the un-enrolled state, which the LED has a colour for.
esp_err_t Init();
bool Ready();

bool Enrolled();
const Enrolment &Current();

// **This device's public key, in the spelling §6 registers**: base64 of the
// derived key's 33-byte compressed point, which is exactly what `lib/crypto.py`
// verifies a `key_type: "p256"` signature with.
//
// Empty when nothing is enrolled, and that is load-bearing rather than tidy: with
// no key there is nothing to register, so `registrar` refuses instead of
// publishing a key that could never sign (§10.7). **Enrolment comes before
// registration on this device**, which is the one ordering §10.18.1 changed.
const char *PublicKeyBase64();

// The boot check of §10.18: re-derive from the stored `ikm`, `ctx` and seed key and
// compare with the public key that was registered.
//
// It costs one ECDH, two scalar multiplications and a point addition, and it
// answers the question nothing else can: whether the file on disk still describes
// the key the handler has in its allowlist. A false here means every signature this
// device could produce would be rejected — so it refuses to subscribe rather than
// discovering it one request at a time.
bool DerivationHolds();

// Is a key on the port right now. A pass-through to the transport, here so that
// callers include one header rather than two.
bool Present();
usb::DeviceInfo Device();

// `authenticatorGetInfo`, for the console. Needs no touch.
esp_err_t Info(ctap2::Info *out, usb::Fault *fault, uint8_t *status);

// **One touch, and this device has a signing key.** A `makeCredential` carrying
// `previewSign.generateKey`, then the derivation, then the file.
//
// Overwrites any previous enrolment on success and changes nothing on failure — the
// same ordering `registration.json` uses, and for the same reason: a failed attempt
// must not cost a working setup.
//
// **It invalidates the registration**, and that is not a bug to be smoothed over:
// the derived key is new, so the allowlist entry naming the old one is worthless
// and a fresh token is needed (§6, §10.18.1). The console says so where it prints
// the new key.
esp_err_t Enrol(uint32_t timeout_ms, usb::Fault *fault, uint8_t *status);

// **The derivation, against a compiled-in vector, with no key on the port.**
//
// The pure half of `components/arkg` is host-tested (§10.11 tier 2); the ECDH and
// the point addition are not, because they need the chip. This is the check that
// covers them: the same seed key, `ikm` and `ctx` the generator used, run through
// PSA and mbedTLS here, compared with the public key Python computed.
//
// It is a console command (`key selftest`) because it is the one part of §10.18
// that can be verified today, on the board, with nothing plugged in — and if it
// fails, no key would ever have worked.
bool SelfTest(char *detail, size_t detail_size);

// Deletes `fido.json`. The credential is left on the key — this firmware has no
// way to remove one, and pretending otherwise would leave an operator believing
// a slot was freed. `keyforget` on the console says so.
esp_err_t Forget();

// **The gate, and now also the signature.** `digest` is 32 bytes the caller
// derived from what it wants signed — `SHA-256(protocol::DecisionSigningBytes)`.
// The caller does that hashing rather than this component, because the bytes belong
// to the protocol and this component has never heard of one.
//
// The same 32 bytes are used twice, and that is deliberate: as the assertion's
// `clientDataHash`, so the key's *own* signature commits to this request, and as
// the extension's `tbs`, which is what comes back as the verdict's signature. Two
// signatures, one request, and neither can be lifted onto another one.
//
// On `kApproved`, `signature` holds the DER ECDSA signature §7 publishes and
// `signature_length` says how long it is — **already verified against the
// registered public key** before this function returns. Every other outcome writes
// nothing there: §10.10's rule is that a gate which did not produce a verifiable
// signature produces no reply at all.
//
// Blocks for up to `timeout_ms` waiting for a fingertip. `keep_alive` is called
// roughly twice a second while waiting; returning false from it cancels.
Gate Sign(const uint8_t *digest, uint32_t timeout_ms, usb::KeepAlive keep_alive,
          void *keep_alive_context, uint8_t *signature, size_t signature_capacity,
          size_t *signature_length, usb::Fault *fault, uint8_t *status);

// Counters, for `key` on the console.
struct Stats {
    uint32_t asked = 0;
    uint32_t approved = 0;
    uint32_t timed_out = 0;
    uint32_t bad_signature = 0;
    uint32_t wrong_key = 0;
    uint32_t enrolments = 0;
};

Stats GetStats();

}  // namespace fido
