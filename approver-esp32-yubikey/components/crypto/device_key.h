#pragma once

// **The device's identity** — the Ed25519 key it signs decisions with
// (CLAUDE.md §10.6).
//
// Two routes to the same key, and **which one shipped is a question this file
// has to answer out loud**, because §10.6 says so and because they are not
// equally good:
//
//   1. **the eFuse route, and the one §10.6 wants.** Nothing is stored: the seed
//      is recomputed at every boot by asking the HMAC unit to authenticate one
//      fixed label with a key that lives in an eFuse block whose *purpose* makes
//      it unreadable by software.
//
//          seed = HMAC-SHA256(efuse key, "ai-remote-approver-esp32-v1")
//
//      A flash dump yields the public key and nothing else, a reboot lands on
//      the same identity so a registration survives it, and the same firmware on
//      another board is a different responder — because that board is not this
//      key.
//
//   2. **the stored-seed route, which is what is running today.** No eFuse key
//      is burned on this board, so the seed is generated once with a true random
//      source and kept in NVS. §10.6 calls this "strictly worse" and it is: the
//      key **is** in flash, and until flash encryption is burned NVS is as
//      readable as SPIFFS, so `esptool read_flash` gives up the signing key and
//      not just the network password. It is honest as a milestone and it is
//      named as one — §10.15 keeps `nvs_keys` reserved and empty for exactly
//      this branch.
//
// Then:
//
//     pair = crypto_sign_seed_keypair(seed)
//
// and everything above this file is identical either way, which is the point of
// the seam being here.
//
// **The eFuse wins when both exist, and it takes the identity with it.** A board
// that has been running on a stored seed and then has a key burned into it is a
// *different responder* afterwards, and needs a fresh registration token (§6).
// That is stated rather than smoothed over, and the stale seed is deleted when it
// happens — the whole cost of route 2 is a private key sitting in flash, and once
// route 1 works there is no reason to keep paying it.
//
// **It is library layer, and it does not know what an approval is** (§10.14.2).
// It signs bytes somebody else assembled; `components/protocol` is where those
// bytes get their meaning, and nothing here mentions a tool, a verdict or a
// subject.
//
// Three things a caller has to know, and each of them has cost somebody an hour
// somewhere:
//
//   * **signing needs a task with room.** `crypto_sign` uses 4,112 bytes of
//     stack, measured on this board; the main task ships with 3,584 and the
//     first probe of this library panicked inside libsodium with a stack
//     protection fault. `kSignStackBytes` below is that number plus honest
//     headroom, and it is what any task that calls `Sign` has to be sized
//     against. Nothing on this board has a callback shallow enough to be tempted
//     — there is no graphics library and no display task (§10.1) — but the number
//     is the constraint either way, and this is the second reason it is written
//     down.
//   * **`Ready()` is not a formality.** A device with no eFuse key burned, or one
//     whose self-test failed, must not subscribe and must say so — on this board
//     with the light (§10.6, §10.17). Every failure here ends in silence rather
//     than in a
//     signature nobody can verify, which is §10.10's rule.
//   * **`Init` runs before the radio and needs nothing from it.** Ed25519
//     signing is deterministic, so signing consumes no entropy — the RNG caveat
//     of §10.7 applies to the registration nonce and not to the key. Generating
//     a seed *does* need real randomness, and route 2 gets it without waiting
//     for the radio by switching the SAR-ADC entropy source on around the one
//     call that needs it, which is the mechanism `bootloader_random_enable`
//     documents for exactly this case. Which is also why `Init` has to run
//     before anything touches the ADC or the radio — `main.cpp` says so where
//     it calls this.

#include <cstddef>
#include <cstdint>

#include "esp_err.h"

namespace crypto {

inline constexpr size_t kSeedSize = 32;
inline constexpr size_t kPublicKeySize = 32;
inline constexpr size_t kSecretKeySize = 64;  // libsodium's sk is seed || pub
inline constexpr size_t kSignatureSize = 64;

// Base64 with padding, plus the terminator — the spelling `lib/crypto.py` uses
// and therefore the only one that can be compared by eye against the handler's
// stderr (§10.7's trust on first use).
inline constexpr size_t kPublicKeyB64Size = 45;  // 32 bytes -> 44 characters
inline constexpr size_t kSignatureB64Size = 89;  // 64 bytes -> 88 characters

// **The stack any task that calls `Sign` must be able to spare.** Measured
// rather than guessed: 4,112 bytes with `CONFIG_LIBSODIUM_USE_MBEDTLS_SHA` set,
// 4,128 without it, taken as a high-water mark against a 16 KB task. The margin
// on top is for whatever the caller is doing around the call.
inline constexpr size_t kSignStackBytes = 6144;

// The label the eFuse key authenticates. **Changing this rotates the key**, and
// deliberately: §10.6 makes that a felt cost, because it invalidates the
// registration and needs a fresh token from the handler (§6).
inline constexpr char kKeyLabel[] = "ai-remote-approver-esp32-v1";

// Why this device can or cannot sign. The failures are separate values because
// they need separate answers from whoever is holding the board: one is a build
// problem, one is hardware, and one is a filesystem that could not keep a seed.
enum class KeyState : uint8_t {
    kUninitialised,   // `Init` has not run
    kSelfTestFailed,  // libsodium disagrees with `lib/crypto.py` - refuse everything
    kHmacFailed,      // an eFuse key is burned and the HMAC unit would not use it
    kNoSeed,          // no key, and none could be made or kept
    kReady,
};

// Where the key came from, which is the difference between §10.6's design and
// §10.6's fallback. `keys` prints it because a device whose private key is in
// flash and one whose private key cannot be read at all are the same device from
// the outside, and only one of them is what this section set out to build.
enum class KeySource : uint8_t {
    kNone,
    kEfuse,       // route 1 - derived per boot, nothing stored
    kStoredSeed,  // route 2 - a 32-byte seed in NVS, readable from a flash dump
};

// Runs the self-test, then derives the key. Safe to call twice; the second call
// answers with the first one's result.
//
// **Call it from a task with at least `kSignStackBytes` free.** It returns
// `ESP_OK` only when `State()` is `kReady`; every other outcome is a device that
// stays silent on the bus, which is the safe end of §10.10.
esp_err_t Init();

KeyState State();
KeySource Source();
bool Ready();

// One line each for a console or a screen, in words rather than section numbers
// (§10.7). Never null.
const char *StateText();
const char *SourceText();

// True when the key came from a seed in flash — the fallback rather than the
// design. A caller that wants to warn about it should not have to compare enum
// values to find out, and the screen of §10.8.5 will want exactly this question.
bool KeyIsInFlash();

// Valid only while `Ready()`. The base64 form is built once at `Init`.
const uint8_t *PublicKey();
const char *PublicKeyBase64();

// Which eFuse block the seed came from, or -1 when it did not come from one.
// `keys` prints it because "which one did it use" is unanswerable from outside
// otherwise.
int EfuseBlock();

// **Deletes the stored seed, if there is one.** The device keeps signing with the
// key it already derived — this changes nothing until the next boot, which then
// makes a fresh seed and a fresh identity. So it is a key rotation with a felt
// cost: the registration it invalidates needs a new token from the handler (§6),
// the same as §10.8.5's `forget`.
//
// Exists for one honest reason: a device leaving the desk should not carry a
// signing key in its flash, and route 2 is the only route where there is
// something to delete.
esp_err_t ForgetStoredSeed();

// Ed25519 over `message`. Deterministic — the same bytes always produce the same
// signature, which is what makes the self-test possible at all (§10.6).
//
// Fails without touching `out` when the device has no key.
esp_err_t Sign(const uint8_t *message, size_t length, uint8_t out[kSignatureSize]);

// The other direction, with a caller-supplied key: §10.7 verifies the
// registration handler's reply before reading a single field out of it, and §6's
// server key is Ed25519 by fixed protocol. Static because it has nothing to do
// with this device's own identity — a device with no key of its own can still
// check somebody else's signature.
bool Verify(const uint8_t public_key[kPublicKeySize], const uint8_t *message, size_t length,
            const uint8_t signature[kSignatureSize]);

// **The boot self-test of §10.6**, exposed so the console can re-run it.
//
// A miscompiled crypto library is silent: the hook would reject every reply and
// Claude Code would keep asking in its own terminal, which from the operator's
// side is indistinguishable from a device that is simply not answering. So a
// fixed key signs a fixed message and the result is compared against bytes
// `lib/crypto.py` produced, and the verify runs against a signature Python made.
// Ed25519 being deterministic is the whole reason this check exists.
bool SelfTest();

// The message `ProveKey` signs. **Chosen so that it can never be §7's signing
// bytes**: those begin with the protocol version's digits and a `\n`, and this
// begins with a letter, so no rearrangement of it is a decision about anything.
inline constexpr char kProofMessage[] = "approver-esp32 key check v1";

// Signs `kProofMessage` with **this device's** key and writes the base64.
//
// `SelfTest` checks the library against a fixed key; this checks the key the
// device actually has, which is the other half of the same question — a seed
// derived from the wrong place, or a keypair whose public and private halves do
// not match, would pass the first and fail this. It is also how the signature can
// be checked against `lib/crypto.py` from the host, using the public key printed
// next to it.
//
// **It is not a signing oracle**, and the reason is the message rather than a
// permission check: there is no argument, so nothing a caller says can change
// what gets signed. §10.10's "the only path to allow is a human press" survives
// this existing, which is why the console may have it.
bool ProveKey(char *out, size_t out_size);

// Base64 in the one spelling this repository uses everywhere: standard alphabet,
// padded. `Encode` returns false only when `out_size` is too small.
bool Base64Encode(const uint8_t *data, size_t length, char *out, size_t out_size);

// Returns the number of bytes written, or 0 when the text is not valid base64 or
// does not fit. Strict on purpose — a pinned server key that decoded to
// something shorter than it should be is a key that would verify nothing.
size_t Base64Decode(const char *text, uint8_t *out, size_t out_size);

}  // namespace crypto
