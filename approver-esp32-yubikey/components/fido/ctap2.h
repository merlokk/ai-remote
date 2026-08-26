#pragma once

// **CTAP2, the three commands this device sends and the three answers it reads**
// (CLAUDE.md §10.18.4).
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
//   * **`authenticatorMakeCredential`** — run once, from `key enrol`, and what it
//     produces is **two** keys: the credential this device will assert with, and
//     the ARKG seed key it derives its signing key from (§10.18.1). The second is
//     why there are two builders for it below;
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

// COSE algorithm identifiers. **ES256 for the credential itself**: every FIDO2
// key supports it, the chip has an ECDSA P-256 verifier already (mbedTLS), and
// offering a second algorithm would mean a second verification path for no gain.
inline constexpr int64_t kAlgEs256 = -7;

// And the three the `previewSign` extension deals in (§10.18). All three are
// **placeholder values in a draft**: they are what `fido2.cose` uses today and
// what a YubiKey on 5.8.x answers to, and they are the first thing to re-check
// when `lib/yubikey.py`'s pinned `fido2` moves.
//
//   * `kAlgArkgP256` is the *seed* key's algorithm — one key pair on the
//     authenticator, from which public keys are derived offline;
//   * `kAlgEsp256` is what the derived keys are: ECDSA P-256 over SHA-256, fully
//     specified, which is exactly `lib/crypto.py`'s `key_type="p256"`;
//   * `kAlgEsp256SplitArkg` is what goes in a request. It names the *split*
//     scheme, where the private half is reconstructed from a key handle instead of
//     being stored anywhere.
inline constexpr int64_t kAlgArkgP256 = -65700;
inline constexpr int64_t kAlgEsp256 = -9;
inline constexpr int64_t kAlgEsp256SplitArkg = -65539;

// The extension's name on the wire. `previewSign` rather than `sign` because the
// draft is a preview — a key that does not advertise it in `getInfo` cannot be
// enrolled on this device at all, and `key info` is where that gets reported
// rather than being discovered in the middle of an approval.
inline constexpr const char *kSignExtension = "previewSign";

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

// The `previewSign` key handle — the credential id of the *generated* key, which
// is what the authenticator needs back before it will reconstruct a derived
// private half. A YubiKey's is far smaller than this; the ceiling is what the
// enrolment file stores, and a key that would exceed it fails enrolment loudly
// rather than being truncated into something that can never sign.
inline constexpr size_t kMaxKeyHandleSize = 256;

// The ARKG seed key as COSE: two nested P-256 keys and four small integers.
inline constexpr size_t kMaxSeedKeySize = 256;

// COSE_Sign_Args — `{3: alg, -1: kh, -2: ctx}`, with ARKG's 81-byte key handle and
// a context at its 64-byte ceiling.
inline constexpr size_t kMaxSignArgsSize = 192;

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
// the storage, kept here in `fido.json` — so enrolling this device
// costs none of a YubiKey's twenty-five resident slots.
bool BuildMakeCredential(const uint8_t *client_data_hash, const uint8_t *user_id, uint8_t *out,
                         size_t capacity, size_t *length);

// `authenticatorMakeCredential` **with `previewSign.generateKey`** — the enrolment
// this device actually runs (§10.18). The plain form above plus one extension map,
// `{3: [kAlgEsp256SplitArkg], 4: 1}`: an algorithm list of one, and flags saying
// user verification is not required, which this device could not satisfy anyway.
//
// What comes back that the plain form does not have is an **ARKG seed key** and a
// key handle for it — `ParseGeneratedKey` below. Everything else in §10.18 hangs
// off that key.
bool BuildMakeCredentialArkg(const uint8_t *client_data_hash, const uint8_t *user_id, uint8_t *out,
                             size_t capacity, size_t *length);

// `authenticatorGetAssertion`, the gate. The credential id from enrolment goes
// in the allow list, `up` is asked for explicitly, and the client data hash is
// the request's own bytes — which is what binds the fingertip to *this* approval
// rather than to the fact that a key is plugged in (§10.18).
bool BuildGetAssertion(const uint8_t *client_data_hash, const uint8_t *credential_id,
                       size_t credential_id_length, uint8_t *out, size_t capacity, size_t *length);

// **The gate as it actually runs**: `getAssertion` carrying `previewSign`, which
// asks the key to sign `tbs` with the derived private key it rebuilds from
// `key_handle` and `args` (§10.18).
//
// `tbs` is a 32-byte digest and it is signed **as-is** — the authenticator does not
// hash it again. That is the single easiest thing to get wrong here, and it is why
// this parameter is a digest rather than a message.
//
// The `client_data_hash` is still the request's own challenge and the allow list
// still names the credential, so the assertion's *own* signature and its
// user-presence flag are unchanged: they remain the proof that a human touched this
// key for this request. The extension adds the verdict's signature; it does not
// replace the evidence.
bool BuildGetAssertionSign(const uint8_t *client_data_hash, const uint8_t *credential_id,
                           size_t credential_id_length, const uint8_t *key_handle,
                           size_t key_handle_length, const uint8_t *tbs, const uint8_t *args,
                           size_t args_length, uint8_t *out, size_t capacity, size_t *length);

// COSE_Sign_Args: `{3: kAlgEsp256SplitArkg, -1: key_handle, -2: ctx}`, handed to
// the authenticator as `additionalArgs` so it can rebuild the private half.
//
// **`key_handle` here is ARKG's `kh`** — the KEM ciphertext the derivation
// produced — and not the credential-shaped key handle `BuildGetAssertionSign`
// takes. The words are nearly the same and the bytes are not; §10.18 puts the two
// side by side for that reason.
bool BuildSignArgs(const uint8_t *key_handle, size_t key_handle_length, const char *ctx,
                   size_t ctx_length, uint8_t *out, size_t capacity, size_t *length);

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

    // Present only when `flags & kFlagExtensionData`. **This is where an
    // assertion's `previewSign` signature arrives** (§10.18) — inside the bytes the
    // key signed over, which is what makes it evidence rather than a value bolted
    // onto a response. A CBOR map keyed by extension *name*, so text keys.
    const uint8_t *extensions = nullptr;
    size_t extensions_length = 0;
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

// What `previewSign.generateKey` produced, out of a `makeCredential` response.
//
// It arrives by an odd road and the shape is worth stating: the response's key 6 is
// a map of **unsigned** extension outputs, its `previewSign` entry's key 7 is a
// whole nested attestation object, and the generated key is that object's
// authenticator data — `credentialId` is the key handle, and the credential public
// key is the ARKG seed key. "Unsigned" means it is not covered by the credential's
// attestation, which costs nothing here: this device verifies no attestation
// (§10.18), and what binds the seed key to anything is that only this authenticator
// can ever use it.
struct GeneratedKey {
    const uint8_t *key_handle = nullptr;
    size_t key_handle_length = 0;
    const uint8_t *seed_cose_key = nullptr;
    size_t seed_cose_key_length = 0;
};

bool ParseGeneratedKey(const uint8_t *response, size_t size, GeneratedKey *out, uint8_t *status);

// The two points inside an ARKG seed key, each `kP256PointSize` bytes. Refuses
// anything whose algorithm is not `kAlgArkgP256`, or whose halves are not P-256
// keys: a seed key this firmware cannot read must not reach the derivation as
// coordinates it would use anyway.
bool ParseArkgSeedKey(const uint8_t *cose_key, size_t size, uint8_t *blinding, uint8_t *kem);

// The signature the `previewSign` extension put inside an assertion's authenticator
// data. **Not the assertion's own signature** — that one is `Assertion::signature`
// and proves presence. This one is the verdict's, made with the derived key, and §7
// is what verifies it.
bool ParseSignSignature(const uint8_t *auth_data, size_t auth_data_length,
                        const uint8_t **signature, size_t *signature_length);

// --- The COSE key ---------------------------------------------------------

// Pulls the uncompressed P-256 point out of a COSE_Key map: `04 || X(32) || Y(32)`,
// which is the form mbedTLS's `mbedtls_ecp_point_read_binary` takes. Refuses
// anything that is not `kty: EC2, alg: ES256, crv: P-256`, because a key type
// this firmware cannot verify must not reach the verifier as bytes it will try
// to interpret.
inline constexpr size_t kP256PointSize = 65;
bool CoseToP256Point(const uint8_t *cose_key, size_t size, uint8_t *out, size_t capacity);

}  // namespace ctap2
