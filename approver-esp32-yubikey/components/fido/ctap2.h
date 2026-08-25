#pragma once

// **CTAP2, the three commands this device sends and the three answers it reads**
// (CLAUDE.md §10.18.2).
//
// Everything here is bytes in and bytes out: a request map built into a caller's
// buffer, a response buffer picked apart into pointers back into itself. No USB,
// no allocation, no clock, no key material — the transport is `ctaphid.h` and the
// decision to trust an answer is `fido.h`'s.
//
// The three commands, and why each one exists on a device with no screen:
//
//   * **`authenticatorGetInfo`** — asked once when a key is plugged in, so that
//     `keyinfo` on the console can name what is on the port and so that a key
//     that cannot do CTAP2 is reported as such rather than failing later inside
//     an approval;
//   * **`authenticatorMakeCredential`** — run once, from `keyenrol`, and what it
//     produces is the credential this device will ask for from then on. §10.18
//     argues why enrolment is a separate step from registration (§10.7) even
//     though both happen when the device is first set up;
//   * **`authenticatorGetAssertion`** — the gate itself. One per approval, with
//     the request's own bytes as the challenge, and it does not return until a
//     finger has touched the key.
//
// **What is deliberately absent: PIN.** `pinUvAuthProtocol` and the whole
// `clientPIN` command family are not here. §10.18 has the argument in full; the
// short form is that a PIN needs a keyboard, this device has one button, and a
// key enrolled without user verification still requires **presence** — which is
// the property this gate is actually built on.

#include <cstddef>
#include <cstdint>

namespace ctap2 {

// The command bytes that lead a `CTAPHID_CBOR` message.
enum Command : uint8_t {
    kMakeCredential = 0x01,
    kGetAssertion = 0x02,
    kGetInfo = 0x04,
    kClientPin = 0x06,
    kReset = 0x07,
};

// The status byte that leads every response. Only the ones this firmware reports
// differently are named — the rest reach the operator as a number, which is what
// they would search for.
enum Status : uint8_t {
    kOk = 0x00,
    kErrInvalidCommand = 0x01,
    kErrInvalidParameter = 0x02,
    kErrInvalidLength = 0x03,
    kErrCborUnexpectedType = 0x11,
    kErrInvalidCbor = 0x12,
    kErrMissingParameter = 0x14,
    kErrCredentialExcluded = 0x19,
    kErrUnsupportedAlgorithm = 0x26,
    kErrOperationDenied = 0x27,
    kErrPinAuthInvalid = 0x33,
    kErrPinRequired = 0x36,
    kErrUpRequired = 0x37,
    kErrNoCredentials = 0x2E,
    kErrKeepaliveCancel = 0x2D,
    kErrActionTimeout = 0x3A,
    kErrUserActionPending = 0x23,
};

const char *StatusName(uint8_t status);

// The relying party this device enrols and asserts under. **A constant, not a
// setting**: it is half of what binds a credential to *this* firmware, and an
// operator who can change it can move a credential to a device that was not the
// one enrolled. It matches `key_id` in the protocol on purpose (§10.2), so that
// a key seen in a browser's credential list names the thing it belongs to.
inline constexpr const char *kRelyingPartyId = "approver-esp32-yubikey";
inline constexpr const char *kRelyingPartyName = "Claude permission approver";

// The user handle the enrolment records. A device, not a person — there is no
// account here, and inventing an e-mail address to put in a credential list is
// worse than saying what it actually is.
inline constexpr const char *kUserName = "device";
inline constexpr const char *kUserDisplayName = "approver-esp32-yubikey";
inline constexpr size_t kUserIdSize = 16;

// COSE algorithm identifiers. **ES256 only.** Every FIDO2 key supports it, the
// chip has an ECDSA P-256 verifier already (mbedTLS), and offering a second
// algorithm would mean a second verification path for no gain.
inline constexpr int64_t kAlgEs256 = -7;

// The flags byte inside authenticator data.
inline constexpr uint8_t kFlagUserPresent = 0x01;
inline constexpr uint8_t kFlagUserVerified = 0x04;
inline constexpr uint8_t kFlagAttestedData = 0x40;
inline constexpr uint8_t kFlagExtensionData = 0x80;

inline constexpr size_t kRpIdHashSize = 32;
inline constexpr size_t kClientDataHashSize = 32;
inline constexpr size_t kAuthDataMinSize = kRpIdHashSize + 1 + 4;

// Credential ids are up to 1023 bytes by the spec; every key that matters emits
// far less (a YubiKey's is 64 for a non-discoverable credential). 256 is the
// ceiling this firmware stores, and a key that would exceed it fails enrolment
// loudly rather than being truncated into something that will never assert.
inline constexpr size_t kMaxCredentialIdSize = 256;

// An uncompressed P-256 point is 65 bytes; the COSE map around it adds about 12.
inline constexpr size_t kMaxCoseKeySize = 128;

// A DER-encoded ECDSA P-256 signature is at most 72 bytes.
inline constexpr size_t kMaxSignatureSize = 80;

// --- Requests -------------------------------------------------------------

// `authenticatorGetInfo` — one byte, and it is here as a function anyway so that
// every request in this firmware is built the same way.
bool BuildGetInfo(uint8_t *out, size_t capacity, size_t *length);

// `authenticatorMakeCredential`, for enrolment. `user_id` is
// `kUserIdSize` bytes the caller generated; storing it is what lets a key with
// several credentials tell this one apart.
//
// **`rk` is false and `uv` is not asked for.** A non-discoverable credential
// means the key stores nothing per-device — the credential id this returns *is*
// the storage, kept here in `registration.json` — so enrolling this device
// costs none of a YubiKey's twenty-five resident slots.
bool BuildMakeCredential(const uint8_t *client_data_hash, const uint8_t *user_id, uint8_t *out,
                         size_t capacity, size_t *length);

// `authenticatorGetAssertion`, the gate. The credential id from enrolment goes
// in the allow list, `up` is asked for explicitly, and the client data hash is
// the request's own bytes — which is what binds the fingertip to *this* approval
// rather than to the fact that a key is plugged in (§10.18).
bool BuildGetAssertion(const uint8_t *client_data_hash, const uint8_t *credential_id,
                       size_t credential_id_length, uint8_t *out, size_t capacity, size_t *length);

// `authenticatorGetAssertion` with no allow list — a **discoverable** credential
// asked for by relying party alone. Used only by `keytest` on the console, for
// the case where somebody wants to know whether a key answers at all without
// enrolling it first.
bool BuildGetAssertionAny(const uint8_t *client_data_hash, uint8_t *out, size_t capacity,
                          size_t *length);

// --- Responses ------------------------------------------------------------

// What a key says about itself. Strings point into the caller's response buffer
// and are **not** null-terminated; the console copies what it needs.
struct Info {
    bool fido2 = false;      // "FIDO_2_0" or later is in `versions`
    bool fido21 = false;
    bool u2f = false;        // "U2F_V2"
    const uint8_t *aaguid = nullptr;  // 16 bytes, or null
    bool option_rk = false;           // can store discoverable credentials
    bool option_up = false;           // can test user presence
    bool option_uv = false;           // …and user verification
    bool client_pin_set = false;      // a PIN exists on this key
    uint64_t max_message_size = 0;
};

// Parses a `CTAPHID_CBOR` response body — **status byte included**, because the
// status is part of what the caller has to see. Returns false on a non-zero
// status or on anything malformed; `*status` is filled either way.
bool ParseInfo(const uint8_t *response, size_t size, Info *out, uint8_t *status);

// The pieces of authenticator data this firmware looks at. `rp_id_hash` and the
// rest point into the caller's buffer.
struct AuthData {
    const uint8_t *rp_id_hash = nullptr;
    uint8_t flags = 0;
    uint32_t sign_count = 0;

    // Present only when `flags & kFlagAttestedData` — i.e. after a
    // `makeCredential`, never after an assertion.
    const uint8_t *credential_id = nullptr;
    size_t credential_id_length = 0;
    const uint8_t *cose_key = nullptr;
    size_t cose_key_length = 0;
};

bool ParseAuthData(const uint8_t *data, size_t size, AuthData *out);

// What `makeCredential` produced: the credential to store, and the public key to
// check its future assertions against.
struct Credential {
    const uint8_t *id = nullptr;
    size_t id_length = 0;
    const uint8_t *cose_key = nullptr;
    size_t cose_key_length = 0;
    const uint8_t *aaguid = nullptr;
    uint8_t flags = 0;
};

bool ParseMakeCredential(const uint8_t *response, size_t size, Credential *out, uint8_t *status);

// What `getAssertion` produced. **`auth_data` and `signature` are what a caller
// verifies**, and this function deliberately does not verify anything: it is a
// parser, and mixing "the bytes were shaped right" with "the signature was good"
// into one boolean is how a parser ends up being trusted.
struct Assertion {
    const uint8_t *credential_id = nullptr;
    size_t credential_id_length = 0;
    const uint8_t *auth_data = nullptr;
    size_t auth_data_length = 0;
    const uint8_t *signature = nullptr;
    size_t signature_length = 0;
    uint64_t number_of_credentials = 0;
};

bool ParseAssertion(const uint8_t *response, size_t size, Assertion *out, uint8_t *status);

// --- The COSE key ---------------------------------------------------------

// Pulls the uncompressed P-256 point out of a COSE_Key map: `04 || X(32) || Y(32)`,
// which is the form mbedTLS's `mbedtls_ecp_point_read_binary` takes. Refuses
// anything that is not `kty: EC2, alg: ES256, crv: P-256`, because a key type
// this firmware cannot verify must not reach the verifier as bytes it will try
// to interpret.
inline constexpr size_t kP256PointSize = 65;
bool CoseToP256Point(const uint8_t *cose_key, size_t size, uint8_t *out, size_t capacity);

}  // namespace ctap2
