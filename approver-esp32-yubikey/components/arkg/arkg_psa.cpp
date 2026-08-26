// **The curve and the hash, on the chip** (CLAUDE.md §10.18, §10.4).
//
// `arkg.cpp` next door is the derivation and includes nothing; this is the four
// primitives it asks for, and the only file in the component that has ESP-IDF
// under it. Keeping the split means five of the derivation's seven steps are host
// tested against Python's own numbers, and the two that are not are these.
//
// **PSA Crypto for three of them, and that is ESP-IDF v6 talking.** On v6 the
// mbedTLS in the tree is the TF-PSA-Crypto one and the classic entry points have
// moved under `mbedtls/private/`; `fido.cpp` states the argument at its own hash
// and it is the same one here. So the SHA-256, the scalar multiplication and the
// ECDH are `psa_*` calls.
//
// **And one of them cannot be.** `pk' = pk_bl + tau * G` is a point addition, and
// PSA has no entry point for adding two public points — it is not an operation any
// protocol PSA was designed for needs. mbedTLS does: `mbedtls_ecp_muladd`, which
// computes `m*P + n*Q` and is exactly this with both scalars at one. It is reached
// through `mbedtls/ecp.h`, which on this install is **ESP-IDF's own shim** in
// `components/mbedtls/port/include` — the framework declares
// `MBEDTLS_DECLARE_PRIVATE_IDENTIFIERS` and includes the private header itself, so
// this is the framework's supported path to it rather than a reach around one.
// `CONFIG_MBEDTLS_ECP_C` and `CONFIG_MBEDTLS_ECP_DP_SECP256R1_ENABLED` are both on
// in `sdkconfig.defaults`' inherited defaults, and `build.md` §10.4 records what
// the whole thing costs.
//
// Nothing here is secret. The scalars this file multiplies by are derived from
// `ikm` in the open, the points are public keys, and the private half of the
// derived key is never on this chip at all — it exists only inside the security
// key, which is the property §10.18 is built on.

#include <cstring>

#include "arkg.h"
#include "mbedtls/ecp.h"
#include "psa/crypto.h"

namespace arkg {
namespace {

bool PsaSha256(const uint8_t *data, size_t length, uint8_t *out) {
    size_t produced = 0;
    if (psa_hash_compute(PSA_ALG_SHA_256, data, length, out, kDigestSize, &produced) !=
        PSA_SUCCESS) {
        return false;
    }
    return produced == kDigestSize;
}

// A private key imported for one operation and destroyed straight after — the same
// discipline `fido.cpp` states for the public key it verifies with, and here it
// matters more: this scalar is a derived one, and a handle left behind is a slot
// that outlives the reason it existed.
bool ImportScalar(const uint8_t *scalar, psa_algorithm_t algorithm, psa_key_usage_t usage,
                  mbedtls_svc_key_id_t *key) {
    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attributes, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
    psa_set_key_bits(&attributes, 256);
    psa_set_key_usage_flags(&attributes, usage);
    psa_set_key_algorithm(&attributes, algorithm);
    const psa_status_t status = psa_import_key(&attributes, scalar, kScalarSize, key);
    psa_reset_key_attributes(&attributes);
    return status == PSA_SUCCESS;
}

bool PsaMulBase(const uint8_t *scalar, uint8_t *out) {
    mbedtls_svc_key_id_t key = MBEDTLS_SVC_KEY_ID_INIT;
    // **No usage flag, and that is not an oversight.** Exporting the *public* half
    // of a key pair needs no permission in PSA; asking for one would be asking for
    // a capability this call does not use.
    if (!ImportScalar(scalar, PSA_ALG_ECDH, 0, &key)) {
        return false;
    }
    uint8_t point[kPointSize];
    size_t produced = 0;
    const psa_status_t status = psa_export_public_key(key, point, sizeof point, &produced);
    psa_destroy_key(key);
    if (status != PSA_SUCCESS || produced != kPointSize || point[0] != 0x04) {
        return false;
    }
    std::memcpy(out, point, kPointSize);
    return true;
}

bool PsaEcdh(const uint8_t *scalar, const uint8_t *peer_point, uint8_t *out) {
    if (peer_point[0] != 0x04) {
        return false;
    }
    mbedtls_svc_key_id_t key = MBEDTLS_SVC_KEY_ID_INIT;
    if (!ImportScalar(scalar, PSA_ALG_ECDH, PSA_KEY_USAGE_DERIVE, &key)) {
        return false;
    }
    // **The raw agreement, which is the X coordinate and nothing else** — RFC
    // 6090's compact output, which is what the draft means by ECDH(pk, sk). A KDF
    // wrapped around it here would be a second key derivation on top of the one
    // the draft already specifies.
    uint8_t secret[kScalarSize];
    size_t produced = 0;
    const psa_status_t status = psa_raw_key_agreement(PSA_ALG_ECDH, key, peer_point, kPointSize,
                                                     secret, sizeof secret, &produced);
    psa_destroy_key(key);
    if (status != PSA_SUCCESS || produced != kScalarSize) {
        return false;
    }
    std::memcpy(out, secret, kScalarSize);
    return true;
}

bool PsaPointAdd(const uint8_t *a, const uint8_t *b, uint8_t *out) {
    if (a[0] != 0x04 || b[0] != 0x04) {
        return false;
    }

    mbedtls_ecp_group group;
    mbedtls_ecp_point left;
    mbedtls_ecp_point right;
    mbedtls_ecp_point sum;
    mbedtls_mpi one;

    mbedtls_ecp_group_init(&group);
    mbedtls_ecp_point_init(&left);
    mbedtls_ecp_point_init(&right);
    mbedtls_ecp_point_init(&sum);
    mbedtls_mpi_init(&one);

    bool ok = mbedtls_ecp_group_load(&group, MBEDTLS_ECP_DP_SECP256R1) == 0 &&
              mbedtls_mpi_lset(&one, 1) == 0 &&
              mbedtls_ecp_point_read_binary(&group, &left, a, kPointSize) == 0 &&
              mbedtls_ecp_point_read_binary(&group, &right, b, kPointSize) == 0 &&
              // `1*P + 1*Q`, which is the addition. `muladd` also rejects a point
              // that is not on the curve, so the check the reads above make is not
              // the only one standing between a bad seed key and a bad answer.
              mbedtls_ecp_muladd(&group, &sum, &one, &left, &one, &right) == 0;

    if (ok) {
        size_t produced = 0;
        uint8_t point[kPointSize];
        ok = mbedtls_ecp_point_write_binary(&group, &sum, MBEDTLS_ECP_PF_UNCOMPRESSED, &produced,
                                            point, sizeof point) == 0 &&
             produced == kPointSize;
        if (ok) {
            std::memcpy(out, point, kPointSize);
        }
    }

    mbedtls_mpi_free(&one);
    mbedtls_ecp_point_free(&sum);
    mbedtls_ecp_point_free(&right);
    mbedtls_ecp_point_free(&left);
    mbedtls_ecp_group_free(&group);
    return ok;
}

constexpr Backend kPsa = {PsaSha256, PsaEcdh, PsaMulBase, PsaPointAdd};

}  // namespace

const Backend *PsaBackend() { return &kPsa; }

}  // namespace arkg
