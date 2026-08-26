#pragma once

// **Checking somebody else's signature, and base64** (CLAUDE.md §10.6).
//
// This file used to be `device_key.h` and used to hold an identity: an Ed25519
// key this board signed decisions with, derived from an eFuse where one was
// burned and from a seed in NVS where one was not. **There is no key here any
// more**, and that is the point of §10.18: the verdict is an ECDSA signature the
// security key makes, its private half is reconstructed inside the authenticator
// from a key handle, and nothing on this board can produce a signature at all.
//
// So the largest thing this component does now is *not* exist. What is left is
// the two jobs that never needed a key of our own:
//
//   * **`Verify`** — §6's registration reply is signed by the handler, and the
//     handler's key is Ed25519 **by fixed protocol**. mbedTLS has no EdDSA
//     (§10.4), so libsodium stays for this one call. It is static: it takes the
//     public key it is checking and holds nothing;
//   * **base64** — the wire format of every key and signature in §6 and §7.
//     libsodium's, because it is already linked and because a hand-rolled one is
//     a decoder taking bytes off a network.
//
// **The identity's removal took the last private key off this board's flash**,
// which is the whole reason to do it. §10.6 called the stored seed "strictly
// worse" than an eFuse and it was: NVS is as readable as SPIFFS until flash
// encryption is burned, so `esptool read_flash` gave up a signing key. It now
// gives up a Wi-Fi passphrase and nothing else. `Init` also **erases the seed a
// previous firmware left behind** — deleting the code that reads it would
// otherwise leave the bytes sitting there, which is a key nobody uses and
// everybody can still read.
//
// **It is library layer and it does not know what an approval is** (§10.14.2). It
// checks bytes somebody else assembled; `components/protocol` is where those bytes
// get their meaning.

#include <cstddef>
#include <cstdint>

#include "esp_err.h"

namespace crypto {

// Ed25519, and these are the handler's sizes rather than ours — §6's reply
// carries a 32-byte public key and a 64-byte signature.
inline constexpr size_t kPublicKeySize = 32;
inline constexpr size_t kSignatureSize = 64;

// Base64 of the above, with room for the terminator. libsodium's
// `sodium_base64_ENCODED_LEN` is the authority; these are it, written out so a
// caller can size a buffer without including `sodium.h`.
inline constexpr size_t kPublicKeyB64Size = 45;  // 32 bytes -> 44 characters
inline constexpr size_t kSignatureB64Size = 89;  // 64 bytes -> 88 characters

// Brings libsodium up, erases any seed an older firmware stored, and runs
// `SelfTest`. **Cheap now**: there is no key to derive, so this no longer needs a
// task with room to sign in — the 6 KB stack requirement went with `Sign`.
//
// Returns `ESP_OK` when the self-test passed. Anything else is a device that must
// not register, because it cannot check the handler's reply — which is the safe
// end of §10.10 and the reason `Ready` is still a thing the responder asks about.
esp_err_t Init();

// Whether `Verify` can be trusted — that is, whether `SelfTest` passed. **Not
// "whether a key exists"**, which is what this meant when there was one.
bool Ready();

// One line of English for the console.
const char *StateText();

// Verifies an Ed25519 signature. The only cryptographic thing this component
// does, and the only reason libsodium is still linked.
bool Verify(const uint8_t public_key[kPublicKeySize], const uint8_t *message, size_t length,
            const uint8_t signature[kSignatureSize]);

// **libsodium against a vector Python produced** (§10.11 tier 2), and it is here
// because `CONFIG_LIBSODIUM_USE_MBEDTLS_SHA` is a seam that has historically
// produced valid-looking but wrong results. Two checks, and the second is the one
// people forget: a signature that must verify, and a one-bit-flipped copy of it
// that must not — because a verifier that says yes to everything passes the first
// on its own.
//
// It no longer signs anything: the vector's seed is unused now, and what a broken
// SHA would break is caught by the verify half either way.
bool SelfTest();

// --- base64 ---------------------------------------------------------------

bool Base64Encode(const uint8_t *data, size_t length, char *out, size_t out_size);

// Returns the number of bytes written, or 0. **0 is the only failure**: a decode
// that produced nothing and a decode that failed are the same thing to every
// caller here.
size_t Base64Decode(const char *text, uint8_t *out, size_t out_size);

}  // namespace crypto
