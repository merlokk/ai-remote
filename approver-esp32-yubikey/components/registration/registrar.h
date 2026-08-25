#pragma once

// **Registering this device with the handler** (CLAUDE.md §6, §10.7) — the one
// exchange that has to happen before anything it signs is worth anything.
//
// What it is: publish `{v, token, key_id, pubkey, key_type, nonce, ts}` on
// `registrations`, wait on a private inbox, **verify the handler's Ed25519
// signature before reading a single field of the reply**, and only on a verified
// `ok:true` write `key_id` and the pinned handler key to `registration.json`.
//
// The order is the whole thing, and it is not enforced here: it is enforced in
// `protocol::ParseRegistrationReply`, which takes the verifier as an argument so
// that there is no way to get the fields out without it having run. This file is
// the part that has a socket, a filesystem and a random number generator.
//
// Three rules it keeps that are somebody's sentence made executable:
//
//   * **the nonce is generated after the radio is up.** §10.7: the ESP32's RNG is
//     only a true random source with the radio enabled, and before that it is a
//     PRNG — a predictable nonce gives away exactly the replay protection the
//     nonce exists for. `Register` refuses without a bus connection, which means
//     a client link, which means the radio.
//   * **a rejection changes nothing.** `registration.json` is written on a
//     verified `ok:true` and on nothing else, so a failed attempt cannot clobber
//     a registration that works — the same ordering all three existing responders
//     use.
//   * **trust on first use, and the screen closes it.** With nothing pinned the
//     handler's key is taken on trust and pinned for every registration
//     afterwards; the console prints it so an operator can compare it, once, with
//     what the handler printed at startup. With something pinned, a reply signed
//     by any other key is refused rather than re-pinned.
//
// Logic layer (§10.14.2): it knows what a `key_id` is. What it does not know is
// how to draw anything or what an `approvals.*` subject looks like.
//
// **The file is `registrar.h` and not `registration.h`** because
// `components/protocol` already has one of those and both directories are on the
// include path — a header that quietly includes itself is an hour nobody enjoys.
// The two are the halves §10.14.2 asks for: the bytes over there, the socket and
// the filesystem here.

#include <cstddef>
#include <cstdint>

#include "esp_err.h"

namespace registration {

// The file, and its temp (§10.15). Separate from `config.json` by **lifetime**:
// §10.15's restore button puts settings back and must not cost a registration,
// because a device that comes back on default settings is a minute's work and a
// device that comes back unregistered needs a new token minted on the host.
inline constexpr const char *kPath = "registration.json";
inline constexpr const char *kTempPath = "registration.json.new";

// Longer than any reply the handler sends, and short enough that a flood on an
// open subject costs nothing. Anything bigger is dropped unread (§10.10).
inline constexpr size_t kMaxReplyBytes = 512;

// How long to wait for the handler. Generous: it is a person watching a console,
// and the failure mode of being too quick is an operator who tries again while
// the first reply is still in flight.
inline constexpr uint32_t kReplyTimeoutMs = 10000;

// Loads `registration.json` if there is one. A missing file is **not an error**:
// it is the unregistered state, which §10.8.2 already requires the clock to
// announce.
esp_err_t Init();

bool Registered();

// Empty when not registered. The `key_id` is read back from the file rather than
// assumed, so a file written by a differently-named build is visible instead of
// silently ignored.
const char *KeyId();

// The pinned handler key, base64, or "" — the string §10.7 has an operator
// compare by eye.
const char *ServerKey();

// When it happened, as the handler's own timestamp. 0 when not registered.
int64_t RegisteredTs();

// Runs the exchange. **Blocking**, for as long as `kReplyTimeoutMs` — it is
// called from the console, where somebody has just typed a token and is waiting.
//
// `detail` is filled with one sentence about what happened, in words rather than
// section numbers (§10.7), whether it worked or not. On success it names the
// handler key so the operator can compare it.
//
// Needs a key of its own (§10.6) and a bus connection, and says which is missing
// rather than failing vaguely: those are the two things somebody can act on.
esp_err_t Register(const char *token, char *detail, size_t detail_size);

// Drops the registration and the pinned key. Nothing to forget is not a failure.
//
// **It costs a token**: `clients[key_id]` on the handler still names a key this
// device can no longer prove it has, so a new one has to be minted (§6). §10.8.5
// says the same about its `forget` entry, and it is why both are two-step.
esp_err_t Forget();

}  // namespace registration
