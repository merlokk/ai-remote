// ARKG-Derive-Public-Key (CLAUDE.md §10.18). Bytes in, a public key and a key
// handle out — no CBOR, no ESP-IDF, no allocation, and the curve arrives through
// `Backend` so that everything in this file except two calls runs in §10.11's host
// tier against numbers Python produced.

#include "arkg.h"

#include <cstring>

namespace arkg {
namespace {

// The order of secp256r1's prime-order subgroup, big-endian. `arkg.h` says why the
// field prime is not here: nothing in this file touches a coordinate, only scalars.
constexpr uint8_t kOrder[kScalarSize] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xBC, 0xE6, 0xFA, 0xAD, 0xA7, 0x17, 0x9E, 0x84, 0xF3, 0xB9, 0xCA, 0xC2, 0xFC, 0x63, 0x25, 0x51,
};

// SHA-256's block size, which is HMAC's, and the `s_in_bytes` of RFC 9380's
// `expand_message_xmd`. One constant, three uses, and all three would be wrong
// together if it were wrong.
constexpr size_t kBlockSize = 64;

// The widest message this pipeline ever expands: `ikm` at its ceiling. Bounded
// rather than generic because the buffer is a fixed array and the caller's input is
// checked against `kIkmMax` before it gets here.
constexpr size_t kXmdMsgMax = kIkmMax;

// And the widest thing HMAC is ever asked to authenticate: HKDF-Expand's
// `T(i-1) || info || counter`.
constexpr size_t kHmacDataMax = kDigestSize + kInfoMax + 1;

// **Two levels of "usable", and the split is the point of the file.** The five
// pure steps need a hash and nothing else, so they say so — which is what lets the
// host tier run them with no curve in the build at all (§10.11). `Derive` is the
// one that needs everything, and it checks for everything.
bool Hashable(const Backend *backend) {
    return backend != nullptr && backend->sha256 != nullptr;
}

bool Usable(const Backend *backend) {
    return Hashable(backend) && backend->ecdh != nullptr && backend->mul_base != nullptr &&
           backend->point_add != nullptr;
}

// `dst || I2OSP(LEN(dst), 1)` — RFC 9380's DST_prime. The length byte is the part
// that stops one tag being a prefix of another, and it is the single easiest byte
// in this file to leave out.
size_t DstPrime(const char *dst, size_t dst_length, uint8_t *out, size_t capacity) {
    if (dst_length > 255 || dst_length + 1 > capacity) {
        return 0;
    }
    std::memcpy(out, dst, dst_length);
    out[dst_length] = static_cast<uint8_t>(dst_length);
    return dst_length + 1;
}

// Appends to a bounded string being assembled, keeping the first failure sticky the
// way `cbor::Writer` does: a caller may build a whole label and check once.
bool Append(char *out, size_t capacity, size_t *length, const char *text, size_t text_length) {
    if (*length + text_length > capacity) {
        *length = capacity + 1;  // poison it, so a later check cannot pass
        return false;
    }
    std::memcpy(out + *length, text, text_length);
    *length += text_length;
    return true;
}

// `'<prefix>' || I2OSP(LEN(ctx), 1) || ctx` — the draft's `ctx_bl` and `ctx_kem`.
// **The length prefix is why an empty `ctx` is not the same as no `ctx`**, and a
// derivation that dropped it would agree with every input in the vectors and
// disagree with the answer.
bool ContextLabel(const char *prefix, const uint8_t *ctx, size_t ctx_length, char *out,
                  size_t capacity, size_t *length) {
    const char count = static_cast<char>(ctx_length);
    *length = 0;
    return Append(out, capacity, length, prefix, std::strlen(prefix)) &&
           Append(out, capacity, length, &count, 1) &&
           Append(out, capacity, length, reinterpret_cast<const char *>(ctx), ctx_length);
}

bool GreaterOrEqualOrder(const uint8_t *value) {
    for (size_t i = 0; i < kScalarSize; i++) {
        if (value[i] != kOrder[i]) {
            return value[i] > kOrder[i];
        }
    }
    return true;  // equal counts: n mod n is 0
}

void SubtractOrder(uint8_t *value) {
    int16_t borrow = 0;
    for (size_t i = kScalarSize; i-- > 0;) {
        int16_t digit = static_cast<int16_t>(value[i]) - kOrder[i] - borrow;
        if (digit < 0) {
            digit += 256;
            borrow = 1;
        } else {
            borrow = 0;
        }
        value[i] = static_cast<uint8_t>(digit);
    }
}

bool IsZero(const uint8_t *value, size_t length) {
    uint8_t seen = 0;
    for (size_t i = 0; i < length; i++) {
        seen |= value[i];
    }
    return seen == 0;
}

}  // namespace

const char *StatusName(Status status) {
    switch (status) {
        case Status::kOk:
            return "ok";
        case Status::kNoBackend:
            return "no-backend";
        case Status::kBadArgument:
            return "bad-argument";
        case Status::kBadSeedKey:
            return "bad-seed-key";
        case Status::kHashFailed:
            return "hash-failed";
        case Status::kCurveFailed:
            return "curve-failed";
        case Status::kBadScalar:
            return "bad-scalar";
    }
    return "?";
}

// --- the pure pieces -------------------------------------------------------

bool ReduceModOrder(const uint8_t *value, size_t length, uint8_t *out) {
    if (value == nullptr || out == nullptr || length == 0) {
        return false;
    }

    // **Bit by bit, with one conditional subtraction each**, and that is the whole
    // bignum in this firmware. `r < n` holds at the top of every iteration, so
    // `2r + bit < 2n` and a single subtraction restores it — which is why there is
    // no loop around the subtraction and must never be one.
    uint8_t remainder[kScalarSize] = {};
    for (size_t byte = 0; byte < length; byte++) {
        for (int bit = 7; bit >= 0; bit--) {
            uint16_t carry = (value[byte] >> bit) & 1u;
            for (size_t i = kScalarSize; i-- > 0;) {
                const uint16_t shifted = static_cast<uint16_t>(remainder[i] << 1) | carry;
                remainder[i] = static_cast<uint8_t>(shifted & 0xFFu);
                carry = shifted >> 8;
            }
            // `carry` here is the 257th bit: set means the value is certainly at
            // least the order, whatever the low 256 bits say.
            if (carry != 0 || GreaterOrEqualOrder(remainder)) {
                SubtractOrder(remainder);
            }
        }
    }

    std::memcpy(out, remainder, kScalarSize);
    return true;
}

bool CompressPoint(const uint8_t *point, uint8_t *out) {
    if (point == nullptr || out == nullptr || point[0] != 0x04) {
        return false;
    }
    // The sign bit is the low bit of Y, and Y ends at byte 64.
    out[0] = static_cast<uint8_t>(0x02 | (point[kPointSize - 1] & 1u));
    std::memcpy(out + 1, point + 1, 32);
    return true;
}

bool ExpandMessageXmd(const Backend *backend, const uint8_t *msg, size_t msg_length,
                      const char *dst, size_t dst_length, uint8_t *out, size_t length) {
    if (!Hashable(backend) || out == nullptr || dst == nullptr || length == 0 ||
        msg_length > kXmdMsgMax || (msg == nullptr && msg_length != 0)) {
        return false;
    }

    uint8_t dst_prime[kDstMax + 1];
    const size_t dst_prime_length = DstPrime(dst, dst_length, dst_prime, sizeof dst_prime);
    if (dst_prime_length == 0) {
        return false;
    }

    const size_t blocks = (length + kDigestSize - 1) / kDigestSize;
    if (blocks > 255 || length > 65535) {
        return false;
    }

    // msg_prime = Z_pad || msg || I2OSP(len, 2) || I2OSP(0, 1) || DST_prime
    uint8_t msg_prime[kBlockSize + kXmdMsgMax + 3 + kDstMax + 1];
    size_t offset = 0;
    std::memset(msg_prime, 0, kBlockSize);
    offset += kBlockSize;
    if (msg_length != 0) {
        std::memcpy(msg_prime + offset, msg, msg_length);
        offset += msg_length;
    }
    msg_prime[offset++] = static_cast<uint8_t>((length >> 8) & 0xFFu);
    msg_prime[offset++] = static_cast<uint8_t>(length & 0xFFu);
    msg_prime[offset++] = 0x00;
    std::memcpy(msg_prime + offset, dst_prime, dst_prime_length);
    offset += dst_prime_length;

    uint8_t b_zero[kDigestSize];
    if (!backend->sha256(msg_prime, offset, b_zero)) {
        return false;
    }

    // b_1 = H(b_0 || 01 || DST'), and every block after it is
    // H(strxor(b_0, b_{i-1}) || i || DST'). The missing xor in the first block is
    // spelled out rather than folded into the loop.
    uint8_t block_input[kDigestSize + 1 + kDstMax + 1];
    uint8_t previous[kDigestSize];
    std::memcpy(previous, b_zero, kDigestSize);

    for (size_t i = 1; i <= blocks; i++) {
        for (size_t j = 0; j < kDigestSize; j++) {
            block_input[j] = (i == 1) ? b_zero[j] : static_cast<uint8_t>(b_zero[j] ^ previous[j]);
        }
        block_input[kDigestSize] = static_cast<uint8_t>(i);
        std::memcpy(block_input + kDigestSize + 1, dst_prime, dst_prime_length);
        if (!backend->sha256(block_input, kDigestSize + 1 + dst_prime_length, previous)) {
            return false;
        }
        const size_t written = (i - 1) * kDigestSize;
        const size_t take = (length - written < kDigestSize) ? length - written : kDigestSize;
        std::memcpy(out + written, previous, take);
    }
    return true;
}

bool HashToScalar(const Backend *backend, const uint8_t *msg, size_t msg_length, const char *dst,
                  size_t dst_length, uint8_t *out) {
    uint8_t wide[kHashToFieldBytes];
    if (!ExpandMessageXmd(backend, msg, msg_length, dst, dst_length, wide, sizeof wide)) {
        return false;
    }
    if (!ReduceModOrder(wide, sizeof wide, out)) {
        return false;
    }
    // Zero is not a scalar: `0 * G` is the point at infinity and there is no key
    // there. A 2^-256 event, refused rather than handled.
    return !IsZero(out, kScalarSize);
}

bool HmacSha256(const Backend *backend, const uint8_t *key, size_t key_length,
                const uint8_t *data, size_t data_length, uint8_t *out) {
    if (!Hashable(backend) || out == nullptr || data_length > kHmacDataMax ||
        (key == nullptr && key_length != 0) || (data == nullptr && data_length != 0)) {
        return false;
    }

    uint8_t padded[kBlockSize] = {};
    if (key_length > kBlockSize) {
        if (!backend->sha256(key, key_length, padded)) {
            return false;
        }
    } else if (key_length != 0) {
        std::memcpy(padded, key, key_length);
    }

    uint8_t inner[kBlockSize + kHmacDataMax];
    for (size_t i = 0; i < kBlockSize; i++) {
        inner[i] = static_cast<uint8_t>(padded[i] ^ 0x36);
    }
    if (data_length != 0) {
        std::memcpy(inner + kBlockSize, data, data_length);
    }

    uint8_t digest[kDigestSize];
    if (!backend->sha256(inner, kBlockSize + data_length, digest)) {
        return false;
    }

    uint8_t outer[kBlockSize + kDigestSize];
    for (size_t i = 0; i < kBlockSize; i++) {
        outer[i] = static_cast<uint8_t>(padded[i] ^ 0x5C);
    }
    std::memcpy(outer + kBlockSize, digest, kDigestSize);
    return backend->sha256(outer, sizeof outer, out);
}

bool HkdfExtract(const Backend *backend, const uint8_t *ikm, size_t ikm_length, uint8_t *prk) {
    // **The salt is not absent, it is zeros.** RFC 5869 defines an unset salt as
    // `HashLen` zero bytes, and `cryptography`'s `HKDF(salt=None)` does exactly
    // that — so a device that treated it as an empty key would derive a different
    // PRK and a key nothing can sign for.
    const uint8_t salt[kDigestSize] = {};
    return HmacSha256(backend, salt, sizeof salt, ikm, ikm_length, prk);
}

bool HkdfExpand(const Backend *backend, const uint8_t *prk, const uint8_t *info,
                size_t info_length, uint8_t *out, size_t length) {
    if (out == nullptr || length == 0 || info_length > kInfoMax ||
        length > 255 * kDigestSize) {
        return false;
    }

    uint8_t block[kDigestSize] = {};
    uint8_t input[kDigestSize + kInfoMax + 1];
    size_t written = 0;
    uint8_t counter = 1;

    while (written < length) {
        size_t offset = 0;
        if (counter != 1) {
            std::memcpy(input, block, kDigestSize);
            offset = kDigestSize;
        }
        if (info_length != 0) {
            std::memcpy(input + offset, info, info_length);
            offset += info_length;
        }
        input[offset++] = counter;
        if (!HmacSha256(backend, prk, kDigestSize, input, offset, block)) {
            return false;
        }
        const size_t take = (length - written < kDigestSize) ? length - written : kDigestSize;
        std::memcpy(out + written, block, take);
        written += take;
        counter++;
    }
    return true;
}

// --- the derivation --------------------------------------------------------

Status Derive(const Backend *backend, const SeedKey &seed, const uint8_t *ikm, size_t ikm_length,
              const uint8_t *ctx, size_t ctx_length, Derived *out, Trace *trace) {
    if (!Usable(backend)) {
        return Status::kNoBackend;
    }
    if (out == nullptr || ikm == nullptr || ikm_length < kIkmMin || ikm_length > kIkmMax ||
        ctx_length > kCtxMax || (ctx == nullptr && ctx_length != 0)) {
        return Status::kBadArgument;
    }
    if (seed.blinding[0] != 0x04 || seed.kem[0] != 0x04) {
        return Status::kBadSeedKey;
    }

    Trace local;
    Trace &t = (trace != nullptr) ? *trace : local;
    t = Trace{};

    // ctx_bl / ctx_kem — the two labels everything below is scoped by.
    char ctx_bl[kDstMax];
    char ctx_kem[kDstMax];
    size_t ctx_bl_length = 0;
    size_t ctx_kem_length = 0;
    if (!ContextLabel("ARKG-Derive-Key-BL.", ctx, ctx_length, ctx_bl, sizeof ctx_bl,
                      &ctx_bl_length) ||
        !ContextLabel("ARKG-Derive-Key-KEM.", ctx, ctx_length, ctx_kem, sizeof ctx_kem,
                      &ctx_kem_length)) {
        return Status::kBadArgument;
    }

    // --- KEM-Encaps -------------------------------------------------------
    //
    // The ephemeral key pair is **derived from `ikm`**, not generated: that is what
    // makes the whole derivation reproducible from a stored `ikm`, and it is why
    // `ikm` has an entropy floor rather than a length.
    {
        char dst[kDstMax];
        size_t dst_length = 0;
        if (!Append(dst, sizeof dst, &dst_length, "ARKG-KEM-ECDH-KG.", 17) ||
            !Append(dst, sizeof dst, &dst_length, kDstExtKem, sizeof kDstExtKem - 1)) {
            return Status::kBadArgument;
        }
        if (!HashToScalar(backend, ikm, ikm_length, dst, dst_length, t.ephemeral_scalar)) {
            return Status::kHashFailed;
        }
    }
    if (!backend->mul_base(t.ephemeral_scalar, t.ephemeral_point)) {
        return Status::kCurveFailed;
    }
    if (!backend->ecdh(t.ephemeral_scalar, seed.kem, t.ecdh_secret)) {
        return Status::kCurveFailed;
    }

    uint8_t prk[kDigestSize];
    if (!HkdfExtract(backend, t.ecdh_secret, sizeof t.ecdh_secret, prk)) {
        return Status::kHashFailed;
    }

    // The two HKDF infos, which differ only in one word — `mac` and `shared`. A
    // device that used one for both would produce a key handle a YubiKey rejects
    // and a blinding scalar nothing can reconstruct, in that order.
    char info[kInfoMax];
    size_t info_length = 0;
    if (!Append(info, sizeof info, &info_length, "ARKG-KEM-HMAC-mac.", 18) ||
        !Append(info, sizeof info, &info_length, kDstExtKem, sizeof kDstExtKem - 1) ||
        !Append(info, sizeof info, &info_length, ctx_kem, ctx_kem_length)) {
        return Status::kBadArgument;
    }
    if (!HkdfExpand(backend, prk, reinterpret_cast<const uint8_t *>(info), info_length, t.mac_key,
                    sizeof t.mac_key)) {
        return Status::kHashFailed;
    }

    {
        uint8_t tag[kDigestSize];
        if (!HmacSha256(backend, t.mac_key, sizeof t.mac_key, t.ephemeral_point,
                        sizeof t.ephemeral_point, tag)) {
            return Status::kHashFailed;
        }
        // **Truncated to 128 bits**, which the draft says and which is the one
        // place a full digest would look more careful and be wrong.
        std::memcpy(t.mac_tag, tag, kMacTagSize);
    }

    info_length = 0;
    if (!Append(info, sizeof info, &info_length, "ARKG-KEM-HMAC-shared.", 21) ||
        !Append(info, sizeof info, &info_length, kDstExtKem, sizeof kDstExtKem - 1) ||
        !Append(info, sizeof info, &info_length, ctx_kem, ctx_kem_length)) {
        return Status::kBadArgument;
    }
    if (!HkdfExpand(backend, prk, reinterpret_cast<const uint8_t *>(info), info_length, t.shared,
                    sizeof t.shared)) {
        return Status::kHashFailed;
    }

    // --- BL-PRF and the blinding -----------------------------------------
    {
        char dst[kDstMax];
        size_t dst_length = 0;
        if (!Append(dst, sizeof dst, &dst_length, "ARKG-BL-EC.", 11) ||
            !Append(dst, sizeof dst, &dst_length, kDstExtBl, sizeof kDstExtBl - 1) ||
            !Append(dst, sizeof dst, &dst_length, ctx_bl, ctx_bl_length)) {
            return Status::kBadArgument;
        }
        if (!HashToScalar(backend, t.shared, sizeof t.shared, dst, dst_length, t.tau)) {
            return Status::kHashFailed;
        }
    }

    uint8_t blind[kPointSize];
    if (!backend->mul_base(t.tau, blind)) {
        return Status::kCurveFailed;
    }

    Derived derived;
    if (!backend->point_add(seed.blinding, blind, derived.point)) {
        return Status::kCurveFailed;
    }
    if (!CompressPoint(derived.point, derived.compressed)) {
        return Status::kCurveFailed;
    }
    std::memcpy(derived.key_handle, t.mac_tag, kMacTagSize);
    std::memcpy(derived.key_handle + kMacTagSize, t.ephemeral_point, kPointSize);

    // **Written out only now.** Everything above worked on locals, so a refusal at
    // any step leaves the caller's `Derived` exactly as it was — the same rule
    // `signing.h` and `fido::Enrol` keep, and for the same reason.
    *out = derived;
    return Status::kOk;
}

}  // namespace arkg
