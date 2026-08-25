#pragma once

// **The gate** (CLAUDE.md §10.18): what has to happen between an operator
// deciding and this device signing.
//
// ## What the key is, and what it is not
//
// **The key does not produce the verdict's signature.** That is the device's own
// Ed25519 key (§10.6), the one `registration_handler.py` has in its allowlist and
// the one `hook.verify_reply` checks — exactly as on the C6 board, and nothing
// on the host side changes for this device. Swapping the protocol's signature to
// something a YubiKey emits would have meant a second scheme in `crypto.py`, a
// second registration path, and an ARKG flow (§8) on a microcontroller.
//
// **What the key produces is permission to use that signature.** Before a single
// byte is signed, this component asks the key for an assertion over *the exact
// bytes about to be signed*, and the key will not answer until somebody touches
// it. Three properties fall out of that, and they are the reason this design is
// worth the USB stack:
//
//   * **a touch cannot be replayed onto a different request.** The challenge is
//     `SHA-256(protocol::DecisionSigningBytes)` — the session, the nonce, the
//     tool, the input hash, the verdict and the timestamp. A different request is
//     a different challenge and needs a different touch;
//   * **the device alone cannot approve anything.** Compromised firmware with the
//     Ed25519 seed still needs the physical key present and a finger on it;
//   * **the key alone cannot approve anything either.** It has no idea what NATS
//     is. Both halves, or nothing — which is the whole point of a second factor
//     and is what the C6 board's single button does not have.
//
// ## Two steps, and they are not the same step
//
// §10.7's `register` tells the *handler* about this device. `keyenrol` here tells
// this *device* about a key: one `authenticatorMakeCredential`, one touch, and
// what comes back — a credential id and a P-256 public key — goes in
// `fido.json`. Keeping them apart is deliberate: a re-registration must not cost
// the key enrolment, and re-enrolling on a spare key must not cost the
// registration.
//
// **The credential is not discoverable** (`rk: false`), so enrolling this device
// uses none of a YubiKey's resident slots. The credential id *is* the storage,
// and it lives here.
//
// ## And the rule it inherits
//
// §10.10: no answer is the safe outcome. Every failure below — no key, no
// enrolment, a touch that never came, a signature that will not verify — ends in
// this function returning something other than `kApproved`, the responder
// publishing nothing, and the hook falling back to its own terminal.

#include <cstddef>
#include <cstdint>

#include "ctap2.h"
#include "esp_err.h"
#include "fido_usb.h"

namespace fido {

// The enrolment file (§10.15's neighbourhood, and its own file for the reason
// `registration.json` is its own: three lifetimes, three files). A `config
// restore` does not touch it and neither does a re-registration.
inline constexpr const char *kPath = "fido.json";
inline constexpr const char *kTempPath = "fido.json.new";

// Longer than the JSON this writes with a 256-byte credential id in base64.
inline constexpr size_t kMaxFileSize = 1024;

// What was learned at enrolment and is needed at every assertion afterwards.
struct Enrolment {
    bool present = false;

    uint8_t user_id[ctap2::kUserIdSize] = {};

    uint8_t credential_id[ctap2::kMaxCredentialIdSize] = {};
    size_t credential_id_length = 0;

    // The uncompressed P-256 point, `04 || X || Y`. **This is the whole security
    // value of the file**: without it an assertion could only be checked for
    // being well-formed, and a key that merely answers is not a key that is
    // yours.
    uint8_t public_key[ctap2::kP256PointSize] = {};

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

// Is a key on the port right now. A pass-through to the transport, here so that
// callers include one header rather than two.
bool Present();
usb::DeviceInfo Device();

// `authenticatorGetInfo`, for the console. Needs no touch.
esp_err_t Info(ctap2::Info *out, usb::Fault *fault, uint8_t *status);

// **One touch, and this device is introduced to that key.** Overwrites any
// previous enrolment on success and changes nothing on failure — the same
// ordering `registration.json` uses, and for the same reason: a failed attempt
// must not cost a working setup.
esp_err_t Enrol(uint32_t timeout_ms, usb::Fault *fault, uint8_t *status);

// Deletes `fido.json`. The credential is left on the key — this firmware has no
// way to remove one, and pretending otherwise would leave an operator believing
// a slot was freed. `keyforget` on the console says so.
esp_err_t Forget();

// **The gate itself.** `challenge` is 32 bytes the caller derived from what it is
// about to sign — `SHA-256(protocol::DecisionSigningBytes)`, and the caller does
// that derivation rather than this component, because the bytes belong to the
// protocol and this component has never heard of one.
//
// Blocks for up to `timeout_ms` waiting for a fingertip. `keep_alive` is called
// roughly twice a second while waiting; returning false from it cancels.
Gate RequireTouch(const uint8_t *challenge, uint32_t timeout_ms, usb::KeepAlive keep_alive,
                  void *keep_alive_context, usb::Fault *fault, uint8_t *status);

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
