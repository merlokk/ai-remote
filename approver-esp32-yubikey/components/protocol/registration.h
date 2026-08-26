#pragma once

// **The registration exchange, as bytes** (CLAUDE.md §6, §10.7): the request
// this device publishes on `registrations`, and — the half that matters — the
// reply, checked in an order that is not the caller's to get right.
//
// `signing.h` next door is `<cstdint>`-only; this one is `<cstdint>` **plus
// cJSON**, which keeps it in §10.11's host tier all the same because the host
// tests already compile the real cJSON for `components/config`. What it is not
// allowed to include is the crypto library, and that is the whole design of the
// file — see below.
//
// ## Why the signature check is a callback rather than a call
//
// §10.7 states the rule this file exists to enforce: **verify the handler's
// signature before reading `ok`.** `registrations` is an open subject (§10.3),
// so an unsigned `{"ok":false,"error":"expired"}` from anybody on the LAN would
// otherwise send an operator hunting a problem that does not exist — and an
// unsigned `ok:true` would be worse.
//
// A parse function that handed back the fields and left the checking to its
// caller would make that ordering a discipline. So the verifier comes **in**, as
// a plain function pointer (§10.14.1 — no `std::function`), and is called at the
// one point in the sequence where it belongs. There is no way to get the fields
// out of here without it having run and said yes.
//
// It takes base64 strings rather than bytes on purpose: decoding is libsodium's,
// this file must not link against it, and the seam falls exactly where the layer
// boundary already is. `components/crypto` supplies the real one; the host tests
// supply one that records the message it was handed, which is how "the bytes
// handed to the verifier are §6's signing bytes" becomes an assertion rather
// than a reading of the code.

#include <cstddef>
#include <cstdint>

namespace protocol {

// §6's domain separator, leading every registration-reply signature. The server
// key signs a different kind of message from the responder keys and both are
// `\n`-joined field lists — this prefix is what stops one being replayed as the
// other.
inline constexpr char kRegistrationReplyContext[] = "registration-reply";

// The subject the request goes to (§6).
inline constexpr char kRegistrationSubject[] = "registrations";

// **This device's name in the allowlist** (§10.2). Its own, never shared with
// another responder — two responders under one `key_id` would be two devices the
// hook cannot tell apart, and revoking one would revoke both.
inline constexpr char kKeyId[] = "approver-esp32-yubikey";

// A `key_id` is non-empty and contains no `.`, because the dot is the token's
// separator (§6). This bound is ours: the handler does not impose one, and
// anything longer than this is not a name somebody typed.
inline constexpr size_t kKeyIdMax = 47;

// `<key_id>.<b64 of 32 bytes>` — 44 characters of base64, a dot, and the name.
inline constexpr size_t kTokenMax = kKeyIdMax + 1 + 44;

// **The signature scheme this device registers under** (§10.2, §10.18). A constant
// rather than a field: the allowlist pins the scheme on the verifying side, and a
// device that could choose one could choose a weaker one.
//
// `p256` because the signer is the security key — an ARKG-derived P-256 key, and an
// ECDSA signature the authenticator makes. The Ed25519 spelling this device used
// before signed with a key that lived in its own flash, which is the property
// §10.18 replaced.
inline constexpr char kKeyType[] = "p256";

// Base64 of 32 bytes, with padding: 44 characters. The nonce and the server key are
// exactly this shape.
inline constexpr size_t kB64_32Max = 44;

// Base64 of 64 bytes: 88 characters.
inline constexpr size_t kB64_64Max = 88;

// And the public key's own bound: base64 of a 33-byte compressed point is 44
// characters too. Its own constant because the two are the same number for
// unrelated reasons, and one of them will move first.
inline constexpr size_t kPubkeyMax = 44;

// The handler's `error` is free text it chose. Bounded here rather than trusted
// (§10.10) — what arrives on an open subject is attacker-shaped, and this string
// is going onto a console.
inline constexpr size_t kErrorMax = 95;

// Everything the request carries (§6). Pointers, like `Decision` next door: the
// caller owns the strings.
struct RegistrationRequest {
    int32_t v = 1;
    int64_t ts = 0;
    const char *token = nullptr;
    const char *key_id = nullptr;
    // Base64 of the 33-byte compressed SEC1 point (§10.18) — 44 characters, which
    // is the same width Ed25519's 32 bytes came to, by coincidence rather than by
    // design.
    const char *pubkey = nullptr;
    const char *nonce = nullptr;  // base64, 32 bytes
    // `key_type` is not a field here: §10.2 pins this device to `p256` and the
    // allowlist pins the scheme on the verifying side. Nothing the device sends
    // may choose an algorithm, so nothing here may vary it.
};

// The largest request this can produce, so the caller's buffer can be a fixed
// array (§10.14.1).
inline constexpr size_t kRequestMax = 320;

// Writes the request JSON and returns its length, or 0 — and **writes nothing**
// on a refusal, the rule `signing.h` states for the same reason.
size_t BuildRegistrationRequest(const RegistrationRequest &request, char *out, size_t out_size);

// The bytes the handler signs over its reply (§6). Layout, `\n`-joined:
//
//     "registration-reply", v, ok ("true"/"false"), key_id, nonce, ts, error
//
// `nonce` is echoed from the request, which binds the reply to the exchange that
// asked for it — an old `ok:true` cannot be replayed at a later registration.
// `error` is last for the reason `reason` is last in a decision: it is the only
// free-text field, so as the tail it stays unambiguous.
inline constexpr size_t kReplySigningBytesMax = sizeof(kRegistrationReplyContext) + 11 + 5 +
                                                kKeyIdMax + kB64_32Max + 20 + kErrorMax + 6 + 1;

size_t RegistrationReplySigningBytes(int32_t v, bool ok, const char *key_id, const char *nonce,
                                     int64_t ts, const char *error, char *out, size_t out_size);

// What a verified reply says. Read only when `ParseRegistrationReply` answered
// `kVerified` — and note that `kVerified` is about the *signature*, not about the
// outcome: a signed rejection is a perfectly good reply, and `ok` is what says
// which it was.
struct RegistrationReply {
    bool ok = false;
    int64_t ts = 0;
    char key_id[kKeyIdMax + 1] = {};
    char server_key[kB64_32Max + 1] = {};
    char error[kErrorMax + 1] = {};
};

// Every way a reply can fail to be one. Separate values because they need
// separate sentences on a console: "the handler said no" and "somebody on the
// network said no" are the same words and completely different problems.
enum class ReplyStatus : uint8_t {
    kVerified,
    kNotJson,
    kBadVersion,
    kNoOk,
    kNoServerKey,
    // A valid signature by a **different** key than the one this device already
    // trusts. Not a mis-signed reply — a takeover, and the reason pinning exists.
    kWrongServerKey,
    kNonceMismatch,
    kNoTimestamp,
    kBadFields,
    kTooLong,
    kBadSignature,
};

const char *ReplyStatusText(ReplyStatus status);

// Given a public key, a message and a signature — all base64 except the message —
// does the signature check out? Base64 and Ed25519 are both libsodium's, which is
// why this arrives from outside.
using ReplyVerifier = bool (*)(const char *public_key_b64, const char *message, size_t length,
                               const char *signature_b64, void *user);

// Checks one reply, in §10.7's order, and fills `out` only on `kVerified`.
//
// `expected_nonce` is the one this device sent; `pinned_server_key` is the key it
// already trusts, or `nullptr`/`""` on a first registration — trust on first use,
// which the operator closes by comparing the string on screen with the one the
// handler printed (§10.7).
ReplyStatus ParseRegistrationReply(const char *json, size_t length, const char *expected_nonce,
                                   const char *pinned_server_key, ReplyVerifier verify, void *user,
                                   RegistrationReply *out);

// `<key_id>.<secret>`, split at the **first** dot — the secret is base64 and can
// contain none, but splitting at the last one would be a different rule than the
// handler's. False when there is no dot or the name is empty or too long.
bool ParseToken(const char *token, char *key_id, size_t key_id_size);

}  // namespace protocol
