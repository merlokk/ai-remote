// **The derivation** (CLAUDE.md §10.18, §10.11 tier 2).
//
// `components/arkg` produces the public key this device registers. Nothing on the
// host side ever checks that it matches the private key the security key will
// reconstruct — the hook just rejects every reply, which from the desk looks
// exactly like a device that is not answering. So this suite is the only thing
// standing between a one-byte mistake and a device that silently never approves
// anything again.
//
// **How the curve gets into a host build: it does not.** The two steps that need
// one — the ECDH inside the KEM and the point addition that blinds the key — are
// *replayed* from the generated vectors: the backend below recognises the exact
// scalars and points Python computed and hands back Python's answers. Everything
// between them is the shipping code doing real work on real numbers:
// `expand_message_xmd`, two HKDF expansions, an HMAC truncated to 128 bits, a
// reduction modulo the subgroup order, and the label assembly that scopes all of
// it.
//
// That split is deliberate and it is worth being honest about what it leaves out:
// a backend whose ECDH is wrong passes every test here. The device tier's `key
// selftest` is the other half — it runs the same vector with the real PSA backend,
// on the chip, and it is a command precisely because that check cannot live here.

#include <cstring>
#include <initializer_list>

#include "arkg.h"
#include "cbor.h"
#include "ctap2.h"
#include "sha256_ref.h"
#include "unity.h"
#include "vectors/arkg_vectors.h"

namespace {

// --- the replaying backend -------------------------------------------------

// Which vector the current test is replaying. Set by `Replay`, read by the two
// curve stubs — a plain file-scope pointer rather than a captured lambda, because
// `arkg::Backend` is function pointers by design (§10.14.1).
const arkg_vectors::Derivation *replaying = nullptr;

// Set when a stub was asked for something no vector has an answer for. That is a
// *failure of the test's assumptions*, not of the code under test, and it must not
// look like a curve error — so it is asserted explicitly at the end of each case.
bool asked_for_the_unknown = false;

bool Sha256(const uint8_t *data, size_t length, uint8_t *out) {
    return sha256_ref(data, length, out) == 1;
}

bool Ecdh(const uint8_t *scalar, const uint8_t *peer, uint8_t *out) {
    if (replaying == nullptr ||
        std::memcmp(scalar, replaying->ephemeral_scalar, sizeof replaying->ephemeral_scalar) != 0 ||
        std::memcmp(peer, arkg_vectors::kSeedKemPoint, sizeof arkg_vectors::kSeedKemPoint) != 0) {
        asked_for_the_unknown = true;
        return false;
    }
    std::memcpy(out, replaying->ecdh_secret, sizeof replaying->ecdh_secret);
    return true;
}

bool MulBase(const uint8_t *scalar, uint8_t *out) {
    if (replaying == nullptr) {
        asked_for_the_unknown = true;
        return false;
    }
    if (std::memcmp(scalar, replaying->ephemeral_scalar, 32) == 0) {
        std::memcpy(out, replaying->ephemeral_point, arkg::kPointSize);
        return true;
    }
    if (std::memcmp(scalar, replaying->tau, 32) == 0) {
        std::memcpy(out, replaying->blind_point, arkg::kPointSize);
        return true;
    }
    asked_for_the_unknown = true;
    return false;
}

bool PointAdd(const uint8_t *a, const uint8_t *b, uint8_t *out) {
    if (replaying == nullptr ||
        std::memcmp(a, arkg_vectors::kSeedBlPoint, arkg::kPointSize) != 0 ||
        std::memcmp(b, replaying->blind_point, arkg::kPointSize) != 0) {
        asked_for_the_unknown = true;
        return false;
    }
    std::memcpy(out, replaying->derived_point, arkg::kPointSize);
    return true;
}

constexpr arkg::Backend kReplay = {Sha256, Ecdh, MulBase, PointAdd};

// A backend with the hash and nothing else — for the pure pieces, and to prove
// that `Derive` refuses rather than dereferences a hole.
constexpr arkg::Backend kHashOnly = {Sha256, nullptr, nullptr, nullptr};

arkg::SeedKey TheSeedKey() {
    arkg::SeedKey seed;
    std::memcpy(seed.blinding, arkg_vectors::kSeedBlPoint, sizeof seed.blinding);
    std::memcpy(seed.kem, arkg_vectors::kSeedKemPoint, sizeof seed.kem);
    return seed;
}

const arkg_vectors::Derivation &Vector(const char *name) {
    for (const auto &candidate : arkg_vectors::kDerivations) {
        if (std::strcmp(candidate.name, name) == 0) {
            return candidate;
        }
    }
    TEST_FAIL_MESSAGE("no derivation vector by that name");
    return arkg_vectors::kDerivations[0];
}

// --- SHA-256, before anything trusts it ------------------------------------

void test_arkg_the_reference_hash_agrees_with_fips_180_4(void) {
    // The suite's own foundation. If this is wrong every assertion below fails for
    // a reason that has nothing to do with ARKG, so it is checked first and
    // against the standard's own example rather than against anything in this
    // repository.
    static const uint8_t kAbc[32] = {
        0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea, 0x41, 0x41, 0x40,
        0xde, 0x5d, 0xae, 0x22, 0x23, 0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17,
        0x7a, 0x9c, 0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad,
    };
    uint8_t digest[32] = {};
    TEST_ASSERT_TRUE(Sha256(reinterpret_cast<const uint8_t *>("abc"), 3, digest));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(kAbc, digest, sizeof kAbc);

    // And the standard's other example, the 448-bit one, which is where the tail
    // arithmetic goes wrong first: 56 bytes leaves no room for the length in the
    // same block, so the padding spills into a second one.
    static const char kLong[] = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    static const uint8_t kLongDigest[32] = {
        0x24, 0x8d, 0x6a, 0x61, 0xd2, 0x06, 0x38, 0xb8, 0xe5, 0xc0, 0x26,
        0x93, 0x0c, 0x3e, 0x60, 0x39, 0xa3, 0x3c, 0xe4, 0x59, 0x64, 0xff,
        0x21, 0x67, 0xf6, 0xec, 0xed, 0xd4, 0x19, 0xdb, 0x06, 0xc1,
    };
    TEST_ASSERT_EQUAL_UINT32(56, sizeof kLong - 1);
    TEST_ASSERT_TRUE(Sha256(reinterpret_cast<const uint8_t *>(kLong), sizeof kLong - 1, digest));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(kLongDigest, digest, sizeof kLongDigest);
}

// --- expand_message_xmd ----------------------------------------------------

void test_arkg_expand_message_xmd_matches_the_vectors(void) {
    for (const auto &vector : arkg_vectors::kXmdVectors) {
        uint8_t out[128] = {};
        TEST_ASSERT_TRUE_MESSAGE(
            arkg::ExpandMessageXmd(&kHashOnly, reinterpret_cast<const uint8_t *>(vector.msg),
                                   vector.msg_length, vector.dst, vector.dst_length, out,
                                   vector.out_length),
            vector.name);
        TEST_ASSERT_EQUAL_UINT8_ARRAY_MESSAGE(vector.out, out, vector.out_length, vector.name);
    }
}

void test_arkg_expand_message_xmd_binds_the_length_into_every_byte(void) {
    // RFC 9380's surprise, and the one an implementation that streamed blocks and
    // truncated would fail: the requested length goes into `b_0`, so asking for
    // fewer bytes is not asking for a prefix.
    uint8_t wide[64] = {};
    uint8_t narrow[32] = {};
    TEST_ASSERT_TRUE(arkg::ExpandMessageXmd(&kHashOnly, nullptr, 0, "dst", 3, wide, sizeof wide));
    TEST_ASSERT_TRUE(
        arkg::ExpandMessageXmd(&kHashOnly, nullptr, 0, "dst", 3, narrow, sizeof narrow));
    TEST_ASSERT_NOT_EQUAL(0, std::memcmp(wide, narrow, sizeof narrow));
}

void test_arkg_expand_message_xmd_separates_a_dst_from_its_own_prefix(void) {
    // DST' is the tag followed by its length, which is what stops "ARKG-P256" and
    // "ARKG-P2567" being the same domain. Leaving that byte out is the single
    // easiest omission in the file.
    uint8_t first[48] = {};
    uint8_t second[48] = {};
    TEST_ASSERT_TRUE(arkg::ExpandMessageXmd(&kHashOnly, nullptr, 0, "ARKG-P256", 9, first,
                                            sizeof first));
    TEST_ASSERT_TRUE(arkg::ExpandMessageXmd(&kHashOnly, nullptr, 0, "ARKG-P2567", 10, second,
                                            sizeof second));
    TEST_ASSERT_NOT_EQUAL(0, std::memcmp(first, second, sizeof first));
}

void test_arkg_expand_message_xmd_refuses_what_it_cannot_hold(void) {
    uint8_t out[32] = {};
    // No hash in the backend at all.
    constexpr arkg::Backend empty = {nullptr, nullptr, nullptr, nullptr};
    TEST_ASSERT_FALSE(arkg::ExpandMessageXmd(&empty, nullptr, 0, "dst", 3, out, sizeof out));
    TEST_ASSERT_FALSE(arkg::ExpandMessageXmd(nullptr, nullptr, 0, "dst", 3, out, sizeof out));
    // A DST past the buffer, and a zero-length request.
    char huge[arkg::kDstMax + 8];
    std::memset(huge, 'x', sizeof huge);
    TEST_ASSERT_FALSE(
        arkg::ExpandMessageXmd(&kHashOnly, nullptr, 0, huge, sizeof huge, out, sizeof out));
    TEST_ASSERT_FALSE(arkg::ExpandMessageXmd(&kHashOnly, nullptr, 0, "dst", 3, out, 0));
    // And a message wider than the pipeline's ceiling: refused rather than cut,
    // because a truncated `ikm` derives a key nothing can sign for.
    uint8_t oversized[arkg::kIkmMax + 1] = {};
    TEST_ASSERT_FALSE(arkg::ExpandMessageXmd(&kHashOnly, oversized, sizeof oversized, "dst", 3,
                                             out, sizeof out));
}

// --- HMAC and HKDF ---------------------------------------------------------

void test_arkg_hmac_matches_rfc4231(void) {
    // Test case 2 from RFC 4231 — a published constant rather than one of ours, so
    // this localises an HMAC mistake without depending on the vectors above it.
    static const uint8_t kExpected[32] = {
        0x5b, 0xdc, 0xc1, 0x46, 0xbf, 0x60, 0x75, 0x4e, 0x6a, 0x04, 0x24,
        0x26, 0x08, 0x95, 0x75, 0xc7, 0x5a, 0x00, 0x3f, 0x08, 0x9d, 0x27,
        0x39, 0x83, 0x9d, 0xec, 0x58, 0xb9, 0x64, 0xec, 0x38, 0x43,
    };
    uint8_t digest[32] = {};
    TEST_ASSERT_TRUE(arkg::HmacSha256(&kHashOnly, reinterpret_cast<const uint8_t *>("Jefe"), 4,
                                      reinterpret_cast<const uint8_t *>("what do ya want for "
                                                                        "nothing?"),
                                      28, digest));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(kExpected, digest, sizeof kExpected);
}

void test_arkg_hmac_hashes_a_key_longer_than_the_block(void) {
    // The branch nothing in this pipeline exercises — every key here is 32 bytes —
    // and the one that silently produces a different MAC if it is missing.
    uint8_t key[100];
    std::memset(key, 0xAA, sizeof key);
    uint8_t hashed[32] = {};
    TEST_ASSERT_TRUE(Sha256(key, sizeof key, hashed));

    uint8_t with_long[32] = {};
    uint8_t with_hashed[32] = {};
    TEST_ASSERT_TRUE(arkg::HmacSha256(&kHashOnly, key, sizeof key,
                                      reinterpret_cast<const uint8_t *>("x"), 1, with_long));
    TEST_ASSERT_TRUE(arkg::HmacSha256(&kHashOnly, hashed, sizeof hashed,
                                      reinterpret_cast<const uint8_t *>("x"), 1, with_hashed));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(with_hashed, with_long, 32);
}

void test_arkg_hkdf_extract_is_an_hmac_under_a_zero_salt(void) {
    // RFC 5869 defines an unset salt as HashLen zeros, which is what
    // `cryptography`'s HKDF(salt=None) does — so extract is exactly one HMAC and
    // this pins which one. What it must not be is the ikm keyed under itself, or
    // the hash of the ikm, both of which produce a plausible 32 bytes and a key
    // handle no authenticator will accept.
    const uint8_t ikm[32] = {1, 2, 3};
    const uint8_t zeros[32] = {};

    uint8_t prk[32] = {};
    uint8_t by_hand[32] = {};
    TEST_ASSERT_TRUE(arkg::HkdfExtract(&kHashOnly, ikm, sizeof ikm, prk));
    TEST_ASSERT_TRUE(arkg::HmacSha256(&kHashOnly, zeros, sizeof zeros, ikm, sizeof ikm, by_hand));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(by_hand, prk, sizeof prk);

    uint8_t self_keyed[32] = {};
    uint8_t hashed[32] = {};
    TEST_ASSERT_TRUE(arkg::HmacSha256(&kHashOnly, ikm, sizeof ikm, ikm, sizeof ikm, self_keyed));
    TEST_ASSERT_TRUE(Sha256(ikm, sizeof ikm, hashed));
    TEST_ASSERT_NOT_EQUAL(0, std::memcmp(self_keyed, prk, sizeof prk));
    TEST_ASSERT_NOT_EQUAL(0, std::memcmp(hashed, prk, sizeof prk));

    // **And "zeros" versus "absent" is not a distinction HMAC can make**, which is
    // worth writing down rather than testing for: a key shorter than the block is
    // zero-padded to it, so a 32-byte zero salt and no salt at all are the same
    // key. The mistake this rules out is therefore a *different* salt, not a
    // missing one.
    uint8_t empty_salt[32] = {};
    TEST_ASSERT_TRUE(arkg::HmacSha256(&kHashOnly, nullptr, 0, ikm, sizeof ikm, empty_salt));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(prk, empty_salt, sizeof prk);
}

void test_arkg_hkdf_expand_chains_its_blocks(void) {
    // Two blocks, where the second takes T(1) as a prefix. A loop that forgot to
    // would return the first block twice.
    const uint8_t prk[32] = {9, 8, 7};
    uint8_t wide[64] = {};
    TEST_ASSERT_TRUE(arkg::HkdfExpand(&kHashOnly, prk, reinterpret_cast<const uint8_t *>("info"),
                                      4, wide, sizeof wide));
    TEST_ASSERT_NOT_EQUAL(0, std::memcmp(wide, wide + 32, 32));

    uint8_t narrow[32] = {};
    TEST_ASSERT_TRUE(arkg::HkdfExpand(&kHashOnly, prk, reinterpret_cast<const uint8_t *>("info"),
                                      4, narrow, sizeof narrow));
    // Unlike expand_message_xmd, HKDF *is* prefix-stable. Both facts are pinned
    // because getting them the wrong way round is a plausible mistake.
    TEST_ASSERT_EQUAL_UINT8_ARRAY(narrow, wide, sizeof narrow);
}

// --- the reduction and the compression -------------------------------------

void test_arkg_reduce_mod_order_handles_the_boundaries(void) {
    static const uint8_t kOrder[32] = {
        0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xBC, 0xE6, 0xFA, 0xAD, 0xA7, 0x17,
        0x9E, 0x84, 0xF3, 0xB9, 0xCA, 0xC2, 0xFC, 0x63, 0x25, 0x51,
    };
    uint8_t out[32] = {};

    // n mod n is zero, which is the case a "subtract while greater" loop written
    // with `>` instead of `>=` gets wrong.
    TEST_ASSERT_TRUE(arkg::ReduceModOrder(kOrder, sizeof kOrder, out));
    for (unsigned char byte : out) {
        TEST_ASSERT_EQUAL_UINT8(0, byte);
    }

    // n - 1 is itself.
    uint8_t below[32];
    std::memcpy(below, kOrder, sizeof below);
    below[31]--;
    TEST_ASSERT_TRUE(arkg::ReduceModOrder(below, sizeof below, out));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(below, out, sizeof out);

    // n + 1 is one.
    uint8_t above[32];
    std::memcpy(above, kOrder, sizeof above);
    above[31]++;
    TEST_ASSERT_TRUE(arkg::ReduceModOrder(above, sizeof above, out));
    for (unsigned i = 0; i < 31; i++) {
        TEST_ASSERT_EQUAL_UINT8(0, out[i]);
    }
    TEST_ASSERT_EQUAL_UINT8(1, out[31]);

    // And the width the pipeline actually uses: 48 bytes of ones, which exercises
    // the carry out of bit 256 on nearly every iteration.
    uint8_t wide[arkg::kHashToFieldBytes];
    std::memset(wide, 0xFF, sizeof wide);
    TEST_ASSERT_TRUE(arkg::ReduceModOrder(wide, sizeof wide, out));
    TEST_ASSERT_EQUAL_INT(-1, std::memcmp(out, kOrder, sizeof out) < 0 ? -1 : 1);
}

void test_arkg_compress_point_keeps_the_parity(void) {
    uint8_t point[arkg::kPointSize];
    std::memset(point, 0x11, sizeof point);
    point[0] = 0x04;
    uint8_t out[arkg::kCompressedSize] = {};

    point[arkg::kPointSize - 1] = 0x02;  // even Y
    TEST_ASSERT_TRUE(arkg::CompressPoint(point, out));
    TEST_ASSERT_EQUAL_UINT8(0x02, out[0]);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(point + 1, out + 1, 32);

    point[arkg::kPointSize - 1] = 0x03;  // odd Y
    TEST_ASSERT_TRUE(arkg::CompressPoint(point, out));
    TEST_ASSERT_EQUAL_UINT8(0x03, out[0]);

    // A compressed point handed back in is not an uncompressed one, and refusing
    // it matters: the compressed form is what gets registered, and a wrong one is
    // a public key no signature will ever match.
    point[0] = 0x02;
    TEST_ASSERT_FALSE(arkg::CompressPoint(point, out));
    TEST_ASSERT_FALSE(arkg::CompressPoint(nullptr, out));
    TEST_ASSERT_FALSE(arkg::CompressPoint(point, nullptr));
}

// --- the whole derivation --------------------------------------------------

void test_arkg_every_vector_derives_byte_for_byte(void) {
    for (const auto &vector : arkg_vectors::kDerivations) {
        replaying = &vector;
        asked_for_the_unknown = false;

        arkg::Derived derived;
        arkg::Trace trace;
        const arkg::Status status =
            arkg::Derive(&kReplay, TheSeedKey(), vector.ikm, sizeof vector.ikm,
                         reinterpret_cast<const uint8_t *>(vector.ctx), vector.ctx_length,
                         &derived, &trace);
        TEST_ASSERT_EQUAL_STRING_MESSAGE("ok", arkg::StatusName(status), vector.name);
        TEST_ASSERT_FALSE_MESSAGE(asked_for_the_unknown, vector.name);

        // Every intermediate, in the order the pipeline produces them, so a
        // failure names the step rather than the outcome.
        TEST_ASSERT_EQUAL_UINT8_ARRAY_MESSAGE(vector.ephemeral_scalar, trace.ephemeral_scalar, 32,
                                              vector.name);
        TEST_ASSERT_EQUAL_UINT8_ARRAY_MESSAGE(vector.ephemeral_point, trace.ephemeral_point, 65,
                                              vector.name);
        TEST_ASSERT_EQUAL_UINT8_ARRAY_MESSAGE(vector.ecdh_secret, trace.ecdh_secret, 32,
                                              vector.name);
        TEST_ASSERT_EQUAL_UINT8_ARRAY_MESSAGE(vector.mac_key, trace.mac_key, 32, vector.name);
        TEST_ASSERT_EQUAL_UINT8_ARRAY_MESSAGE(vector.mac_tag, trace.mac_tag, 16, vector.name);
        TEST_ASSERT_EQUAL_UINT8_ARRAY_MESSAGE(vector.shared, trace.shared, 32, vector.name);
        TEST_ASSERT_EQUAL_UINT8_ARRAY_MESSAGE(vector.tau, trace.tau, 32, vector.name);

        // And the three things a caller keeps.
        TEST_ASSERT_EQUAL_UINT8_ARRAY_MESSAGE(vector.derived_point, derived.point, 65,
                                              vector.name);
        TEST_ASSERT_EQUAL_UINT8_ARRAY_MESSAGE(vector.derived_compressed, derived.compressed, 33,
                                              vector.name);
        TEST_ASSERT_EQUAL_UINT8_ARRAY_MESSAGE(vector.key_handle, derived.key_handle, 81,
                                              vector.name);
    }
    replaying = nullptr;
}

void test_arkg_the_key_handle_is_the_tag_and_the_ephemeral_point(void) {
    // `kh` is what travels back to the authenticator, and its layout is the one
    // thing in the output a caller could get away with reordering here and
    // discover on the day a real key refuses to sign.
    const auto &vector = Vector("default");
    replaying = &vector;
    arkg::Derived derived;
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(arkg::Status::kOk),
        static_cast<int>(arkg::Derive(&kReplay, TheSeedKey(), vector.ikm, sizeof vector.ikm,
                                      reinterpret_cast<const uint8_t *>(vector.ctx),
                                      vector.ctx_length, &derived)));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(vector.mac_tag, derived.key_handle, 16);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(vector.ephemeral_point, derived.key_handle + 16, 65);
    replaying = nullptr;
}

void test_arkg_an_empty_context_is_not_the_absence_of_one(void) {
    // The length prefix. `I2OSP(0, 1) || ""` is one byte, and an implementation
    // that skipped the prefix for an empty ctx agrees with every input in the
    // vectors and disagrees with the answer — which is why there is a vector for
    // it and this test only has to prove the two differ.
    const auto &empty = Vector("empty-ctx");
    const auto &normal = Vector("default");
    TEST_ASSERT_EQUAL_UINT32(0, empty.ctx_length);
    TEST_ASSERT_NOT_EQUAL(0, std::memcmp(empty.derived_point, normal.derived_point, 65));
}

void test_arkg_derive_is_deterministic(void) {
    // The property the whole scheme rests on: a device that reboots re-derives the
    // key it registered. If this were not exact, a reboot would silently produce a
    // responder whose public key nothing has in an allowlist.
    const auto &vector = Vector("default");
    replaying = &vector;

    arkg::Derived first;
    arkg::Derived second;
    for (arkg::Derived *out : {&first, &second}) {
        TEST_ASSERT_EQUAL_INT(
            static_cast<int>(arkg::Status::kOk),
            static_cast<int>(arkg::Derive(&kReplay, TheSeedKey(), vector.ikm, sizeof vector.ikm,
                                          reinterpret_cast<const uint8_t *>(vector.ctx),
                                          vector.ctx_length, out)));
    }
    TEST_ASSERT_EQUAL_UINT8_ARRAY(first.point, second.point, 65);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(first.key_handle, second.key_handle, 81);
    replaying = nullptr;
}

void test_arkg_derive_refuses_and_leaves_the_output_alone(void) {
    // §10.10 reaching this far down. Every refusal below has to leave the caller's
    // `Derived` untouched, because the caller's next move on a failure is to keep
    // whatever it already had — and a half-written public key it might register
    // instead is the one outcome here worse than no key.
    const auto &vector = Vector("default");
    replaying = &vector;

    arkg::Derived derived;
    std::memset(&derived, 0xEE, sizeof derived);
    const arkg::Derived untouched = derived;

    const uint8_t *ctx = reinterpret_cast<const uint8_t *>(vector.ctx);

    // No backend, and a backend with a hole in it.
    TEST_ASSERT_EQUAL_INT(static_cast<int>(arkg::Status::kNoBackend),
                          static_cast<int>(arkg::Derive(nullptr, TheSeedKey(), vector.ikm, 32,
                                                        ctx, vector.ctx_length, &derived)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(arkg::Status::kNoBackend),
                          static_cast<int>(arkg::Derive(&kHashOnly, TheSeedKey(), vector.ikm, 32,
                                                        ctx, vector.ctx_length, &derived)));

    // An ikm below the draft's entropy floor, and one past the buffer.
    TEST_ASSERT_EQUAL_INT(static_cast<int>(arkg::Status::kBadArgument),
                          static_cast<int>(arkg::Derive(&kReplay, TheSeedKey(), vector.ikm,
                                                        arkg::kIkmMin - 1, ctx, vector.ctx_length,
                                                        &derived)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(arkg::Status::kBadArgument),
                          static_cast<int>(arkg::Derive(&kReplay, TheSeedKey(), vector.ikm,
                                                        arkg::kIkmMax + 1, ctx, vector.ctx_length,
                                                        &derived)));

    // A ctx one byte past the draft's ceiling: refused, never truncated.
    uint8_t long_ctx[arkg::kCtxMax + 1] = {};
    TEST_ASSERT_EQUAL_INT(static_cast<int>(arkg::Status::kBadArgument),
                          static_cast<int>(arkg::Derive(&kReplay, TheSeedKey(), vector.ikm, 32,
                                                        long_ctx, sizeof long_ctx, &derived)));

    // A seed key that is not two uncompressed points.
    arkg::SeedKey bad = TheSeedKey();
    bad.kem[0] = 0x02;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(arkg::Status::kBadSeedKey),
                          static_cast<int>(arkg::Derive(&kReplay, bad, vector.ikm, 32, ctx,
                                                        vector.ctx_length, &derived)));

    TEST_ASSERT_EQUAL_UINT8_ARRAY(reinterpret_cast<const uint8_t *>(&untouched),
                                  reinterpret_cast<const uint8_t *>(&derived), sizeof derived);
    replaying = nullptr;
}

void test_arkg_a_curve_that_says_no_is_not_a_key(void) {
    // The stubs refuse anything they have no answer for, so pointing the
    // derivation at the wrong vector's ikm makes the ECDH fail — and the result
    // has to be a status, not a point.
    const auto &vector = Vector("default");
    const auto &other = Vector("empty-ctx");
    replaying = &other;
    asked_for_the_unknown = false;

    arkg::Derived derived;
    const arkg::Status status =
        arkg::Derive(&kReplay, TheSeedKey(), vector.ikm, sizeof vector.ikm,
                     reinterpret_cast<const uint8_t *>(vector.ctx), vector.ctx_length, &derived);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(arkg::Status::kCurveFailed), static_cast<int>(status));
    TEST_ASSERT_TRUE(asked_for_the_unknown);
    replaying = nullptr;
    asked_for_the_unknown = false;
}

void test_arkg_the_device_context_is_the_one_the_responder_uses(void) {
    // §10.18: the same purpose label `approver/responder_yubikey.py` scopes its key
    // to. Pinned here because the vectors are generated from the Python constant
    // and the firmware carries its own copy of the string.
    TEST_ASSERT_EQUAL_STRING(arkg_vectors::kDeviceCtx, arkg::kDeviceCtx);
    TEST_ASSERT_EQUAL_STRING(arkg_vectors::kDstExtBl, arkg::kDstExtBl);
    TEST_ASSERT_EQUAL_STRING(arkg_vectors::kDstExtKem, arkg::kDstExtKem);
    TEST_ASSERT_EQUAL_UINT32(arkg_vectors::kHashToFieldBytes, arkg::kHashToFieldBytes);
}

void test_arkg_every_status_has_a_name(void) {
    for (const arkg::Status status :
         {arkg::Status::kOk, arkg::Status::kNoBackend, arkg::Status::kBadArgument,
          arkg::Status::kBadSeedKey, arkg::Status::kHashFailed, arkg::Status::kCurveFailed,
          arkg::Status::kBadScalar}) {
        TEST_ASSERT_NOT_NULL(arkg::StatusName(status));
        TEST_ASSERT_TRUE(std::strcmp(arkg::StatusName(status), "?") != 0);
    }
}

// --- previewSign on the wire -----------------------------------------------
//
// The requests this device sends and the two answers it reads. These live here
// rather than in `test_ctap2.cpp` because their expectations are *generated*: the
// responses come out of `make_arkg_vectors.py`, and the args map is the one
// `tests/test_esp32yk_arkg_vectors.py` checks against Yubico's own encoder. A
// hand-written expectation would pin one reading of the draft twice.

uint8_t request[1024];

void test_arkg_the_args_map_is_the_one_python_encodes(void) {
    // **The map a wrong byte in is a key that refuses to sign, with a status code
    // that names nothing.** Canonical CBOR orders these three keys `3, -1, -2` by
    // their encoded form; sorting by numeric value would put the negatives first.
    for (const auto &vector : arkg_vectors::kDerivations) {
        size_t length = 0;
        TEST_ASSERT_TRUE_MESSAGE(
            ctap2::BuildSignArgs(vector.key_handle, sizeof vector.key_handle, vector.ctx,
                                 vector.ctx_length, request, sizeof request, &length),
            vector.name);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(vector.args_cbor_length, length, vector.name);
        TEST_ASSERT_EQUAL_UINT8_ARRAY_MESSAGE(vector.args_cbor, request, length, vector.name);
    }
}

void test_arkg_the_args_map_refuses_what_it_cannot_hold(void) {
    const auto &vector = Vector("default");
    size_t length = 0;
    TEST_ASSERT_FALSE(ctap2::BuildSignArgs(nullptr, 0, vector.ctx, vector.ctx_length, request,
                                           sizeof request, &length));
    // A buffer too small is a refusal, never a truncated map — the same rule every
    // builder in `ctap2.cpp` keeps.
    TEST_ASSERT_FALSE(ctap2::BuildSignArgs(vector.key_handle, sizeof vector.key_handle,
                                           vector.ctx, vector.ctx_length, request, 8, &length));
}

void test_arkg_makecredential_asks_for_a_generated_key(void) {
    uint8_t challenge[32];
    std::memset(challenge, 0x5A, sizeof challenge);
    uint8_t user_id[ctap2::kUserIdSize];
    std::memset(user_id, 0x11, sizeof user_id);

    size_t length = 0;
    TEST_ASSERT_TRUE(ctap2::BuildMakeCredentialArkg(challenge, user_id, request, sizeof request,
                                                    &length));
    TEST_ASSERT_EQUAL_UINT8(ctap2::kMakeCredential, request[0]);
    TEST_ASSERT_EQUAL_UINT8(0xA5, request[1]);  // map, five pairs — 1..4 and 6

    // The extension is there by name, and the algorithm asked for is the split
    // ARKG one. Without both, the key makes an ordinary credential and enrolment
    // ends with nothing to derive from — which would look like a key that "worked".
    const uint8_t *body = request + 1;
    const size_t body_size = length - 1;
    const uint8_t *extensions = nullptr;
    size_t left = 0;
    TEST_ASSERT_TRUE(cbor::MapFind(body, body_size, 6, &extensions, &left));
    const uint8_t *entry = nullptr;
    size_t entry_left = 0;
    TEST_ASSERT_TRUE(cbor::MapFindText(extensions, left, ctap2::kSignExtension, &entry,
                                       &entry_left));

    const uint8_t *algorithms = nullptr;
    size_t algorithms_left = 0;
    TEST_ASSERT_TRUE(cbor::MapFind(entry, entry_left, 3, &algorithms, &algorithms_left));
    cbor::Item item;
    TEST_ASSERT_TRUE(cbor::Decode(algorithms, algorithms_left, &item));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(cbor::Type::kArray), static_cast<int>(item.type));
    TEST_ASSERT_EQUAL_UINT32(1, item.value);
    TEST_ASSERT_TRUE(cbor::Decode(algorithms + item.header, algorithms_left - item.header, &item));
    TEST_ASSERT_EQUAL_INT64(ctap2::kAlgEsp256SplitArkg, item.signed_value);

    // Flags 1: user verification **not** required. `0b101` would ask the key to
    // enforce UV on every future signature, which on a device with one button and
    // no PIN pad is an enrolment that can never be used again (§10.18).
    const uint8_t *flags = nullptr;
    size_t flags_left = 0;
    TEST_ASSERT_TRUE(cbor::MapFind(entry, entry_left, 4, &flags, &flags_left));
    uint64_t value = 0;
    TEST_ASSERT_TRUE(cbor::GetUint(flags, flags_left, &value));
    TEST_ASSERT_EQUAL_UINT32(1, value);
}

void test_arkg_the_signing_assertion_carries_the_digest_and_still_demands_a_touch(void) {
    const auto &vector = Vector("default");
    uint8_t challenge[32];
    std::memset(challenge, 0xC3, sizeof challenge);
    uint8_t tbs[32];
    std::memset(tbs, 0x7E, sizeof tbs);

    uint8_t args[ctap2::kMaxSignArgsSize];
    size_t args_length = 0;
    TEST_ASSERT_TRUE(ctap2::BuildSignArgs(vector.key_handle, sizeof vector.key_handle, vector.ctx,
                                          vector.ctx_length, args, sizeof args, &args_length));

    size_t length = 0;
    TEST_ASSERT_TRUE(ctap2::BuildGetAssertionSign(
        challenge, arkg_vectors::kCredentialId, sizeof arkg_vectors::kCredentialId,
        arkg_vectors::kSeedKeyHandle, sizeof arkg_vectors::kSeedKeyHandle, tbs, args, args_length,
        request, sizeof request, &length));

    TEST_ASSERT_EQUAL_UINT8(ctap2::kGetAssertion, request[0]);
    TEST_ASSERT_EQUAL_UINT8(0xA5, request[1]);  // map, five pairs — 1..5

    const uint8_t *body = request + 1;
    const size_t body_size = length - 1;

    // **`up: true` survives the extension.** A signature the key produced without
    // presence would be a verdict nobody made, so this byte matters exactly as much
    // here as it does in the plain assertion.
    const uint8_t *options = nullptr;
    size_t options_left = 0;
    TEST_ASSERT_TRUE(cbor::MapFind(body, body_size, 5, &options, &options_left));
    const uint8_t *up = nullptr;
    size_t up_left = 0;
    TEST_ASSERT_TRUE(cbor::MapFindText(options, options_left, "up", &up, &up_left));
    bool present = false;
    TEST_ASSERT_TRUE(cbor::GetBool(up, up_left, &present));
    TEST_ASSERT_TRUE(present);

    // The three integers inside the extension: 2 = keyHandle, 6 = tbs,
    // 7 = additionalArgs.
    const uint8_t *extensions = nullptr;
    size_t left = 0;
    TEST_ASSERT_TRUE(cbor::MapFind(body, body_size, 4, &extensions, &left));
    const uint8_t *entry = nullptr;
    size_t entry_left = 0;
    TEST_ASSERT_TRUE(cbor::MapFindText(extensions, left, ctap2::kSignExtension, &entry,
                                       &entry_left));

    const uint8_t *found = nullptr;
    size_t found_left = 0;
    const uint8_t *data = nullptr;
    size_t data_length = 0;

    TEST_ASSERT_TRUE(cbor::MapFind(entry, entry_left, 2, &found, &found_left));
    TEST_ASSERT_TRUE(cbor::GetBytes(found, found_left, &data, &data_length));
    TEST_ASSERT_EQUAL_UINT32(sizeof arkg_vectors::kSeedKeyHandle, data_length);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(arkg_vectors::kSeedKeyHandle, data, data_length);

    TEST_ASSERT_TRUE(cbor::MapFind(entry, entry_left, 6, &found, &found_left));
    TEST_ASSERT_TRUE(cbor::GetBytes(found, found_left, &data, &data_length));
    TEST_ASSERT_EQUAL_UINT32(32, data_length);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(tbs, data, 32);

    TEST_ASSERT_TRUE(cbor::MapFind(entry, entry_left, 7, &found, &found_left));
    TEST_ASSERT_TRUE(cbor::GetBytes(found, found_left, &data, &data_length));
    TEST_ASSERT_EQUAL_UINT32(args_length, data_length);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(args, data, args_length);
}

void test_arkg_the_seed_key_parses_into_two_points(void) {
    uint8_t blinding[ctap2::kP256PointSize] = {};
    uint8_t kem[ctap2::kP256PointSize] = {};
    TEST_ASSERT_TRUE(ctap2::ParseArkgSeedKey(arkg_vectors::kSeedKeyCbor,
                                             sizeof arkg_vectors::kSeedKeyCbor, blinding, kem));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(arkg_vectors::kSeedBlPoint, blinding, sizeof blinding);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(arkg_vectors::kSeedKemPoint, kem, sizeof kem);
}

void test_arkg_a_seed_key_of_the_wrong_kind_is_refused(void) {
    // A COSE key whose algorithm is not the ARKG one has its coordinates somewhere
    // else entirely. Reading them anyway would produce a derivation that looked
    // like it worked and a public key nothing can ever sign for.
    uint8_t mangled[sizeof arkg_vectors::kSeedKeyCbor];
    std::memcpy(mangled, arkg_vectors::kSeedKeyCbor, sizeof mangled);

    // Byte 3 onwards is the algorithm's encoding; flipping one bit of it is enough.
    uint8_t blinding[ctap2::kP256PointSize] = {};
    uint8_t kem[ctap2::kP256PointSize] = {};
    for (size_t i = 0; i < sizeof mangled; i++) {
        if (mangled[i] == 0x3A) {  // the negative-int head the alg is written with
            mangled[i + 1] ^= 0x01;
            break;
        }
    }
    TEST_ASSERT_FALSE(ctap2::ParseArkgSeedKey(mangled, sizeof mangled, blinding, kem));

    // And a truncated key, which is what a short read off the cable looks like.
    TEST_ASSERT_FALSE(ctap2::ParseArkgSeedKey(arkg_vectors::kSeedKeyCbor, 20, blinding, kem));
}

void test_arkg_the_generated_key_is_found_in_the_unsigned_outputs(void) {
    ctap2::GeneratedKey generated;
    uint8_t status = 0xFF;
    TEST_ASSERT_TRUE(ctap2::ParseGeneratedKey(arkg_vectors::kMakeCredentialResponse,
                                              arkg_vectors::kMakeCredentialResponseLength,
                                              &generated, &status));
    TEST_ASSERT_EQUAL_UINT8(ctap2::kOk, status);
    TEST_ASSERT_EQUAL_UINT32(sizeof arkg_vectors::kSeedKeyHandle, generated.key_handle_length);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(arkg_vectors::kSeedKeyHandle, generated.key_handle,
                                  generated.key_handle_length);
    TEST_ASSERT_EQUAL_UINT32(sizeof arkg_vectors::kSeedKeyCbor, generated.seed_cose_key_length);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(arkg_vectors::kSeedKeyCbor, generated.seed_cose_key,
                                  generated.seed_cose_key_length);

    // And the credential itself is still read the ordinary way out of the same
    // response — the two live side by side, and mixing them up would enrol the
    // device on a credential it cannot assert with.
    ctap2::Credential credential;
    TEST_ASSERT_TRUE(ctap2::ParseMakeCredential(arkg_vectors::kMakeCredentialResponse,
                                                arkg_vectors::kMakeCredentialResponseLength,
                                                &credential, &status));
    TEST_ASSERT_EQUAL_UINT32(sizeof arkg_vectors::kCredentialId, credential.id_length);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(arkg_vectors::kCredentialId, credential.id, credential.id_length);
    TEST_ASSERT_NOT_EQUAL(0, std::memcmp(credential.id, generated.key_handle,
                                         credential.id_length));
}

void test_arkg_a_response_without_a_generated_key_is_not_an_enrolment(void) {
    // A key that answered a `generateKey` request without generating one leaves
    // this device with a credential it can assert with and no key to derive from.
    // Failing here is what turns that into a refused enrolment instead of a
    // registration nothing can verify.
    ctap2::GeneratedKey generated;
    uint8_t status = 0xFF;

    // The plain assertion response has no key 6 at all.
    TEST_ASSERT_FALSE(ctap2::ParseGeneratedKey(arkg_vectors::kAssertionResponse,
                                               arkg_vectors::kAssertionResponseLength, &generated,
                                               &status));
    // A non-zero status is a failure whatever the body says, and the code survives.
    uint8_t refused[8] = {ctap2::kErrUnsupportedAlgorithm, 0xA0};
    TEST_ASSERT_FALSE(ctap2::ParseGeneratedKey(refused, sizeof refused, &generated, &status));
    TEST_ASSERT_EQUAL_UINT8(ctap2::kErrUnsupportedAlgorithm, status);
    // And a truncated response.
    TEST_ASSERT_FALSE(ctap2::ParseGeneratedKey(arkg_vectors::kMakeCredentialResponse, 40,
                                               &generated, &status));
}

void test_arkg_the_two_signatures_in_an_assertion_are_told_apart(void) {
    // **The one confusion in this whole component that would be silent.** Key 3 is
    // the assertion's own signature, over `authData || clientDataHash`, and it is
    // what proves a human touched the key. The one inside the extensions is the
    // verdict's, made with the derived key, and it is what §7 publishes. Sending
    // the first as the second produces a reply the hook rejects; sending the second
    // as the first makes the presence check verify nothing.
    ctap2::Assertion assertion;
    uint8_t status = 0xFF;
    TEST_ASSERT_TRUE(ctap2::ParseAssertion(arkg_vectors::kAssertionResponse,
                                           arkg_vectors::kAssertionResponseLength, &assertion,
                                           &status));
    TEST_ASSERT_EQUAL_UINT32(sizeof arkg_vectors::kAssertionSignature, assertion.signature_length);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(arkg_vectors::kAssertionSignature, assertion.signature,
                                  assertion.signature_length);

    const uint8_t *signature = nullptr;
    size_t signature_length = 0;
    TEST_ASSERT_TRUE(ctap2::ParseSignSignature(assertion.auth_data, assertion.auth_data_length,
                                               &signature, &signature_length));
    TEST_ASSERT_EQUAL_UINT32(sizeof arkg_vectors::kSignExtensionSignature, signature_length);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(arkg_vectors::kSignExtensionSignature, signature,
                                  signature_length);

    // And the presence flag, which is read off the same authenticator data and is
    // the reason the extension does not replace the assertion.
    ctap2::AuthData auth;
    TEST_ASSERT_TRUE(ctap2::ParseAuthData(assertion.auth_data, assertion.auth_data_length, &auth));
    TEST_ASSERT_TRUE((auth.flags & ctap2::kFlagUserPresent) != 0);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(auth.rp_id_hash, auth.rp_id_hash, 32);
}

void test_arkg_an_assertion_with_no_extension_yields_no_signature(void) {
    // A key that answered without the extension has not signed the verdict. That is
    // a `bad-signature` outcome and no reply (§10.10), never a verdict signed with
    // something else — so the parser must say no rather than fall back.
    ctap2::Assertion assertion;
    uint8_t status = 0;
    TEST_ASSERT_TRUE(ctap2::ParseAssertion(arkg_vectors::kAssertionResponse,
                                           arkg_vectors::kAssertionResponseLength, &assertion,
                                           &status));

    // Authenticator data with the ED flag cleared and the extension map cut off.
    uint8_t stripped[37];
    std::memcpy(stripped, assertion.auth_data, sizeof stripped);
    stripped[32] = ctap2::kFlagUserPresent;

    const uint8_t *signature = reinterpret_cast<const uint8_t *>(1);
    size_t signature_length = 99;
    TEST_ASSERT_FALSE(ctap2::ParseSignSignature(stripped, sizeof stripped, &signature,
                                                &signature_length));
    TEST_ASSERT_NULL(signature);
    TEST_ASSERT_EQUAL_UINT32(0, signature_length);
}

void test_arkg_a_signature_that_could_not_be_one_is_refused(void) {
    // The last place that knows these bytes came off a cable. A four-byte
    // "signature" is not a DER ECDSA one, and a verifier handed it would report a
    // bad signature — which reads as "the wrong key answered" rather than "this key
    // sent nonsense".
    const uint8_t tiny_extensions[] = {
        0xA1, 0x6B, 'p',  'r',  'e',  'v',  'i',  'e',  'w', 'S',
        'i',  'g',  'n',  0xA1, 0x06, 0x44, 0x30, 0x02, 0x02, 0x01,
    };
    uint8_t auth[37 + sizeof tiny_extensions];
    std::memset(auth, 0, 37);
    auth[32] = ctap2::kFlagUserPresent | ctap2::kFlagExtensionData;
    std::memcpy(auth + 37, tiny_extensions, sizeof tiny_extensions);

    const uint8_t *signature = nullptr;
    size_t signature_length = 0;
    TEST_ASSERT_FALSE(ctap2::ParseSignSignature(auth, sizeof auth, &signature, &signature_length));
}

void test_arkg_the_relying_party_is_the_one_the_vectors_were_built_under(void) {
    // A mismatch here means every `rpIdHash` in the generated responses is for
    // another device, and the five checks of §10.18.3 would be testing nothing.
    TEST_ASSERT_EQUAL_STRING(arkg_vectors::kRelyingPartyId, ctap2::kRelyingPartyId);
}

}  // namespace

void RegisterArkgTests(void) {
    RUN_TEST(test_arkg_the_reference_hash_agrees_with_fips_180_4);

    RUN_TEST(test_arkg_expand_message_xmd_matches_the_vectors);
    RUN_TEST(test_arkg_expand_message_xmd_binds_the_length_into_every_byte);
    RUN_TEST(test_arkg_expand_message_xmd_separates_a_dst_from_its_own_prefix);
    RUN_TEST(test_arkg_expand_message_xmd_refuses_what_it_cannot_hold);

    RUN_TEST(test_arkg_hmac_matches_rfc4231);
    RUN_TEST(test_arkg_hmac_hashes_a_key_longer_than_the_block);
    RUN_TEST(test_arkg_hkdf_extract_is_an_hmac_under_a_zero_salt);
    RUN_TEST(test_arkg_hkdf_expand_chains_its_blocks);

    RUN_TEST(test_arkg_reduce_mod_order_handles_the_boundaries);
    RUN_TEST(test_arkg_compress_point_keeps_the_parity);

    RUN_TEST(test_arkg_every_vector_derives_byte_for_byte);
    RUN_TEST(test_arkg_the_key_handle_is_the_tag_and_the_ephemeral_point);
    RUN_TEST(test_arkg_an_empty_context_is_not_the_absence_of_one);
    RUN_TEST(test_arkg_derive_is_deterministic);
    RUN_TEST(test_arkg_derive_refuses_and_leaves_the_output_alone);
    RUN_TEST(test_arkg_a_curve_that_says_no_is_not_a_key);
    RUN_TEST(test_arkg_the_device_context_is_the_one_the_responder_uses);
    RUN_TEST(test_arkg_every_status_has_a_name);

    // previewSign on the wire: the two requests, and the two answers.
    RUN_TEST(test_arkg_the_args_map_is_the_one_python_encodes);
    RUN_TEST(test_arkg_the_args_map_refuses_what_it_cannot_hold);
    RUN_TEST(test_arkg_makecredential_asks_for_a_generated_key);
    RUN_TEST(test_arkg_the_signing_assertion_carries_the_digest_and_still_demands_a_touch);
    RUN_TEST(test_arkg_the_seed_key_parses_into_two_points);
    RUN_TEST(test_arkg_a_seed_key_of_the_wrong_kind_is_refused);
    RUN_TEST(test_arkg_the_generated_key_is_found_in_the_unsigned_outputs);
    RUN_TEST(test_arkg_a_response_without_a_generated_key_is_not_an_enrolment);
    RUN_TEST(test_arkg_the_two_signatures_in_an_assertion_are_told_apart);
    RUN_TEST(test_arkg_an_assertion_with_no_extension_yields_no_signature);
    RUN_TEST(test_arkg_a_signature_that_could_not_be_one_is_refused);
    RUN_TEST(test_arkg_the_relying_party_is_the_one_the_vectors_were_built_under);
}
