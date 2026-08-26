#pragma once

// **SHA-256, for the host tier only** (CLAUDE.md §10.11).
//
// `components/arkg` takes its hash through a function pointer precisely so that
// the six pure steps of the derivation can be tested with no ESP-IDF and no PSA
// Crypto in the build. Something has to supply that pointer here, and this is it:
// FIPS 180-4, written out, about a hundred lines.
//
// **It ships nowhere.** On the device the backend is `arkg_psa.cpp`, which calls
// the mbedTLS the framework already links — there is not a second hash
// implementation in the firmware and there must not be. This file is a test
// fixture, in the same position as `fakes/`, and it is in the parent directory
// rather than in `fakes/` because it is not a fake: it is the real algorithm, and
// the suite pins it against FIPS 180-4's own digests before using it for anything.

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Writes 32 bytes. Returns 1 on success, and can only fail on a null argument —
// the signature matches `arkg::Backend::sha256`'s so it can be handed straight to
// it, and that one takes a bool.
int sha256_ref(const uint8_t *data, size_t length, uint8_t *out);

#ifdef __cplusplus
}
#endif
