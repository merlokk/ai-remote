// The gate (CLAUDE.md §10.18): enrolment on disk, one assertion per approval, and
// the verification that makes the assertion mean something.

#include "fido.h"

#include <cstdio>
#include <cstring>

#include "arkg.h"
#include "cJSON.h"
#include "cbor.h"
#include "ctaphid_frames.h"
#include "device_key.h"
#include "esp_log.h"
#include "esp_random.h"
#include "psa/crypto.h"
#include "arkg_selftest_vector.h"
#include "storage.h"

namespace fido {
namespace {

constexpr const char *TAG = "fido";

// One request and one response buffer, static (§10.14.1).
//
// **The largest request is the signing assertion, and it is sized from the
// ceilings rather than from what a YubiKey happens to send.** `getAssertion` with
// `previewSign` carries four variable things at once — the credential id in the
// allow list, the generated key's handle, the 32-byte digest and COSE_Sign_Args:
//
//     256  kMaxCredentialIdSize    the allow list entry
//     256  kMaxKeyHandleSize       previewSign's key handle
//     192  kMaxSignArgsSize        COSE_Sign_Args, with ctx at its own ceiling
//      32  the digest
//    ~120  rpId, the maps, the keys, `up: true`
//    ----
//     856
//
// A real key's credential id is 64 bytes and its handle is similar, so this is
// several times what any of them will use. Sizing it to *those* numbers would have
// made an unusual key fail at the moment of an approval, with `kBadSignature` and
// nothing naming the cause — the one place in this file where a tight buffer would
// be indistinguishable from a broken key.
constexpr size_t kRequestMax = 1152;
constexpr size_t kResponseMax = ctaphid::kMaxMessage;

uint8_t request_buffer[kRequestMax];
uint8_t response_buffer[kResponseMax];
char file_buffer[kMaxFileSize];

struct Runtime {
    bool ready = false;
    Enrolment enrolment;
    Stats stats;

    // The derived public key in base64, built once at load and at enrolment. §6
    // registers this string and `hook.verify_reply` looks it up; it is cached
    // because `registrar` and the console both want it and neither should be
    // encoding a key.
    char public_key_b64[48] = {};

    // Whether the stored derivation still reproduces the registered key. Checked
    // once, at load — an answer that could change per call would be a device that
    // is sometimes on `approvals.*` (§10.10 rule 5).
    bool derivation_holds = false;
    // SHA-256 of `ctap2::kRelyingPartyId`, computed once at Init. Every assertion
    // is checked against it, so it is worth not recomputing per approval — and
    // worth having in one place rather than at the two call sites.
    uint8_t rp_id_hash[ctap2::kRpIdHashSize] = {};
};

Runtime runtime;

// **PSA Crypto, not `mbedtls_sha256`, and this is ESP-IDF v6 talking.** On v6
// the mbedTLS in the tree is the TF-PSA-Crypto one, and every classic entry point
// — `mbedtls_sha256`, `mbedtls_ecdsa_verify`, `mbedtls_ecp_*` — has moved into
// `mbedtls/private/`. Reaching into a directory called `private` to verify a
// security key's signature would be the wrong kind of clever; PSA is the API that
// is meant to be called, and it happens to be the smaller one for this job.
void Sha256(const uint8_t *data, size_t length, uint8_t *out) {
    size_t produced = 0;
    if (psa_hash_compute(PSA_ALG_SHA_256, data, length, out, 32, &produced) != PSA_SUCCESS ||
        produced != 32) {
        // A hash that did not happen must not look like a hash of nothing.
        std::memset(out, 0, 32);
    }
}

void HexEncode(const uint8_t *data, size_t length, char *out) {
    static const char kDigits[] = "0123456789abcdef";
    for (size_t i = 0; i < length; i++) {
        out[i * 2] = kDigits[data[i] >> 4];
        out[i * 2 + 1] = kDigits[data[i] & 0x0F];
    }
    out[length * 2] = '\0';
}

// **A DER ECDSA signature, parsed by hand.** `SEQUENCE { INTEGER r, INTEGER s }`,
// and that is the whole grammar — every byte of it is checked against the buffer
// rather than against the length the key claimed, which is the same discipline
// `cbor.cpp` states and for the same reason: the producer is on the other end of
// a cable.
//
// **And it has to happen here because PSA will not do it.** `psa_verify_hash`
// takes ECDSA signatures in the raw `r || s` form — 64 bytes for P-256, each half
// fixed-width and big-endian — while CTAP2 hands back the DER the WebAuthn spec
// asks for. So this function is also the converter: `out` receives 64 bytes with
// each integer left-padded, which is where the two conventions meet.
//
// The leading-zero rule is the one that bites: DER writes `02 21 00 ff...` for an
// integer whose top bit is set, and a converter that copies the length verbatim
// produces a 33-byte half that PSA rejects. The padding below is written from the
// *right* for exactly that reason.
bool ParseDerSignature(const uint8_t *der, size_t length, uint8_t *out) {
    if (der == nullptr || out == nullptr || length < 8 || der[0] != 0x30) {
        return false;
    }
    std::memset(out, 0, 64);
    size_t offset = 1;
    size_t body = der[offset++];
    if (body & 0x80) {
        // Long form. A P-256 signature never needs more than one length byte, so
        // anything else is refused rather than decoded.
        if ((body & 0x7F) != 1 || offset >= length) {
            return false;
        }
        body = der[offset++];
    }
    if (offset + body != length) {
        return false;
    }

    for (int half = 0; half < 2; half++) {
        if (offset + 2 > length || der[offset] != 0x02) {
            return false;
        }
        offset++;
        size_t int_length = der[offset++];
        if (int_length == 0 || int_length > 33 || offset + int_length > length) {
            return false;
        }
        const uint8_t *value = der + offset;
        offset += int_length;

        // Drop DER's sign byte, then refuse anything that still will not fit.
        while (int_length > 0 && value[0] == 0x00) {
            value++;
            int_length--;
        }
        if (int_length > 32) {
            return false;
        }
        std::memcpy(out + half * 32 + (32 - int_length), value, int_length);
    }
    return offset == length;
}

// **The check, for both of the signatures this device cares about.** A DER ECDSA
// signature over a 32-byte digest, under an uncompressed P-256 point.
//
// Two callers, and the difference between them is the whole of §10.18:
//
//   * `VerifyAssertion` below, with the credential's public key over
//     `SHA-256(authData || clientDataHash)` — that one proves **a human touched
//     this key for this request**;
//   * `Sign`, with the *derived* public key over the request's own digest — that
//     one is the verdict, and it is what §7 publishes.
//
// **The public key is imported for one verification and destroyed afterwards**,
// rather than kept as a PSA key across the life of the device. It is a public key,
// so nothing is being protected by that — what it buys is that there is no handle to
// leak, no slot to run out of, and no state that can be stale after a `key enrol`
// replaced the enrolment underneath it. Importing costs microseconds next to a
// fingertip.
bool VerifyP256(const uint8_t *point, const uint8_t *digest, const uint8_t *signature,
                size_t signature_length) {
    uint8_t raw_signature[64];
    if (!ParseDerSignature(signature, signature_length, raw_signature)) {
        return false;
    }

    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attributes, PSA_KEY_TYPE_ECC_PUBLIC_KEY(PSA_ECC_FAMILY_SECP_R1));
    psa_set_key_bits(&attributes, 256);
    psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_VERIFY_HASH);
    psa_set_key_algorithm(&attributes, PSA_ALG_ECDSA(PSA_ALG_SHA_256));

    mbedtls_svc_key_id_t key = MBEDTLS_SVC_KEY_ID_INIT;
    if (psa_import_key(&attributes, point, ctap2::kP256PointSize, &key) != PSA_SUCCESS) {
        return false;
    }

    const psa_status_t verified =
        psa_verify_hash(key, PSA_ALG_ECDSA(PSA_ALG_SHA_256), digest, 32, raw_signature,
                        sizeof(raw_signature));
    psa_destroy_key(key);
    return verified == PSA_SUCCESS;
}

// **The one function on this device that turns "a key answered" into "the key
// answered".** Everything around it is plumbing; this is the check.
bool VerifyAssertion(const uint8_t *auth_data, size_t auth_data_length, const uint8_t *challenge,
                     const uint8_t *signature, size_t signature_length) {
    // What FIDO signs is `authData || clientDataHash`, concatenated raw and then
    // hashed by ECDSA itself. Not a nested hash, not a prefix — this is the one
    // detail that silently produces a verifier which rejects every valid
    // signature, so it is spelled out rather than inlined.
    uint8_t digest[32];
    {
        psa_hash_operation_t hash = PSA_HASH_OPERATION_INIT;
        size_t produced = 0;
        const bool hashed =
            psa_hash_setup(&hash, PSA_ALG_SHA_256) == PSA_SUCCESS &&
            psa_hash_update(&hash, auth_data, auth_data_length) == PSA_SUCCESS &&
            psa_hash_update(&hash, challenge, ctap2::kClientDataHashSize) == PSA_SUCCESS &&
            psa_hash_finish(&hash, digest, sizeof(digest), &produced) == PSA_SUCCESS;
        if (!hashed || produced != sizeof(digest)) {
            psa_hash_abort(&hash);
            return false;
        }
    }
    return VerifyP256(runtime.enrolment.public_key, digest, signature, signature_length);
}

// Runs the derivation for an enrolment and fills `derived` — including the ARKG key
// handle, **which the file does not store**. That is the reason this is a function
// and not a load-time nicety: `kh` is what travels back to the authenticator, and
// re-deriving it is the only way to have it. The file keeps the *inputs* and the
// expected answer; everything else is recomputed.
arkg::Status DeriveInto(Enrolment *enrolment) {
    return arkg::Derive(arkg::PsaBackend(), enrolment->seed, enrolment->ikm,
                        sizeof enrolment->ikm,
                        reinterpret_cast<const uint8_t *>(enrolment->ctx),
                        std::strlen(enrolment->ctx), &enrolment->derived);
}

// A base64 blob out of the JSON, into a fixed buffer, with the length pinned. Every
// field in this file is one of these, and the exact-length check is what stops a
// truncated key being loaded as a short one.
bool ReadBlob(const cJSON *root, const char *name, uint8_t *out, size_t expected,
              size_t *written = nullptr) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
    if (!cJSON_IsString(item) || item->valuestring == nullptr) {
        return false;
    }
    const size_t decoded = crypto::Base64Decode(item->valuestring, out, expected);
    if (written != nullptr) {
        *written = decoded;
        return decoded > 0;
    }
    return decoded == expected;
}

esp_err_t Load() {
    if (!storage::Mounted()) {
        return ESP_ERR_INVALID_STATE;
    }
    storage::RecoverInterruptedWrite(kPath, kTempPath);
    if (!storage::Exists(kPath)) {
        return ESP_ERR_NOT_FOUND;
    }

    size_t length = 0;
    const esp_err_t err = storage::ReadFile(kPath, file_buffer, sizeof(file_buffer), &length);
    if (err != ESP_OK) {
        return err;
    }

    cJSON *root = cJSON_Parse(file_buffer);
    if (root == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    Enrolment loaded;
    bool good = true;

    // **The format version, first.** A v1 file is the pre-ARKG shape: a credential
    // and no seed key, so there is no signing key in it at all. It is treated as no
    // enrolment rather than migrated — there is nothing to migrate *from* (nothing
    // ever wrote one on hardware), and a device that half-loaded one would be a
    // device that cannot sign and does not know it.
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, "v");
    if (!cJSON_IsNumber(item) || static_cast<int>(item->valuedouble) != kFormatVersion) {
        cJSON_Delete(root);
        ESP_LOGW(TAG, "%s is not format %d; this device is not enrolled", kPath, kFormatVersion);
        return ESP_ERR_INVALID_VERSION;
    }

    // The credential half — what proves a human touched this key.
    good = good && ReadBlob(root, "credentialId", loaded.credential_id,
                            sizeof loaded.credential_id, &loaded.credential_id_length);
    good = good && ReadBlob(root, "publicKey", loaded.public_key, sizeof loaded.public_key);
    // The point has to be an uncompressed one or the verifier will not take it.
    // Checked at load rather than at the first approval, so a corrupted file is a
    // boot log line and not a refused verdict.
    good = good && loaded.public_key[0] == 0x04;
    good = good && ReadBlob(root, "userId", loaded.user_id, sizeof loaded.user_id);

    // The signing half — what makes the verdict.
    good = good && ReadBlob(root, "keyHandle", loaded.key_handle, sizeof loaded.key_handle,
                            &loaded.key_handle_length);
    good = good && ReadBlob(root, "seedBlinding", loaded.seed.blinding,
                            sizeof loaded.seed.blinding);
    good = good && ReadBlob(root, "seedKem", loaded.seed.kem, sizeof loaded.seed.kem);
    good = good && ReadBlob(root, "ikm", loaded.ikm, sizeof loaded.ikm);

    item = cJSON_GetObjectItemCaseSensitive(root, "ctx");
    if (cJSON_IsString(item) && item->valuestring != nullptr &&
        std::strlen(item->valuestring) <= arkg::kCtxMax) {
        snprintf(loaded.ctx, sizeof loaded.ctx, "%s", item->valuestring);
    } else {
        good = false;
    }

    // The answer this file claims. Kept **only** to be checked against a fresh
    // derivation below: it is not trusted as the key, it is what the key has to
    // agree with.
    uint8_t claimed[arkg::kCompressedSize] = {};
    good = good && ReadBlob(root, "derivedPublicKey", claimed, sizeof claimed);

    item = cJSON_GetObjectItemCaseSensitive(root, "aaguid");
    if (cJSON_IsString(item) && item->valuestring != nullptr) {
        snprintf(loaded.aaguid, sizeof loaded.aaguid, "%s", item->valuestring);
    }
    item = cJSON_GetObjectItemCaseSensitive(root, "vendorId");
    if (cJSON_IsNumber(item)) {
        loaded.vendor_id = static_cast<uint16_t>(item->valuedouble);
    }
    item = cJSON_GetObjectItemCaseSensitive(root, "productId");
    if (cJSON_IsNumber(item)) {
        loaded.product_id = static_cast<uint16_t>(item->valuedouble);
    }

    cJSON_Delete(root);
    if (!good) {
        ESP_LOGW(TAG, "%s is not usable; this device is not enrolled", kPath);
        return ESP_ERR_INVALID_ARG;
    }

    // **And now the derivation, which is most of what loading means here.** It
    // produces the ARKG key handle — the file does not store it — and the public
    // key, and that key has to be the one the file says was registered. A mismatch
    // means the file was edited or the derivation changed underneath it, and either
    // way every signature this device could make would be rejected by the hook.
    // Hence a refusal at boot rather than a discovery on the first request.
    const arkg::Status status = DeriveInto(&loaded);
    if (status != arkg::Status::kOk) {
        ESP_LOGE(TAG, "the stored key would not re-derive (%s); this device is not enrolled",
                 arkg::StatusName(status));
        return ESP_ERR_INVALID_STATE;
    }
    if (std::memcmp(loaded.derived.compressed, claimed, sizeof claimed) != 0) {
        ESP_LOGE(TAG,
                 "%s no longer derives the key it registered - a reply signed now would be "
                 "rejected. Re-enrol, then re-register.",
                 kPath);
        return ESP_ERR_INVALID_STATE;
    }

    loaded.present = true;
    if (!crypto::Base64Encode(loaded.derived.compressed, sizeof loaded.derived.compressed,
                              runtime.public_key_b64, sizeof runtime.public_key_b64)) {
        runtime.public_key_b64[0] = 0;
        return ESP_ERR_INVALID_SIZE;
    }
    runtime.enrolment = loaded;
    runtime.derivation_holds = true;
    return ESP_OK;
}

esp_err_t Save(const Enrolment &enrolment) {
    // Base64 of the widest thing each field can hold, plus room for padding and a
    // terminator.
    char credential_b64[ctap2::kMaxCredentialIdSize * 2 + 4];
    char public_b64[ctap2::kP256PointSize * 2 + 4];
    char user_b64[ctap2::kUserIdSize * 2 + 4];
    char handle_b64[ctap2::kMaxKeyHandleSize * 2 + 4];
    char blinding_b64[arkg::kPointSize * 2 + 4];
    char kem_b64[arkg::kPointSize * 2 + 4];
    char ikm_b64[sizeof enrolment.ikm * 2 + 4];
    char derived_b64[arkg::kCompressedSize * 2 + 4];

    if (!crypto::Base64Encode(enrolment.credential_id, enrolment.credential_id_length,
                              credential_b64, sizeof credential_b64) ||
        !crypto::Base64Encode(enrolment.public_key, ctap2::kP256PointSize, public_b64,
                              sizeof public_b64) ||
        !crypto::Base64Encode(enrolment.user_id, ctap2::kUserIdSize, user_b64, sizeof user_b64) ||
        !crypto::Base64Encode(enrolment.key_handle, enrolment.key_handle_length, handle_b64,
                              sizeof handle_b64) ||
        !crypto::Base64Encode(enrolment.seed.blinding, arkg::kPointSize, blinding_b64,
                              sizeof blinding_b64) ||
        !crypto::Base64Encode(enrolment.seed.kem, arkg::kPointSize, kem_b64, sizeof kem_b64) ||
        !crypto::Base64Encode(enrolment.ikm, sizeof enrolment.ikm, ikm_b64, sizeof ikm_b64) ||
        !crypto::Base64Encode(enrolment.derived.compressed, arkg::kCompressedSize, derived_b64,
                              sizeof derived_b64)) {
        return ESP_ERR_INVALID_SIZE;
    }

    // Hand-written rather than through cJSON, because none of these fields can hold
    // a character that would need escaping — base64, hex, a label this firmware
    // chose, and two integers. The one thing that has to be right is that the file
    // is valid JSON, and a test asserts it round-trips.
    //
    // **`ikm` is in here and it is not a signing secret.** Anyone holding it plus
    // the seed public key can derive this device's *public* key, which is already on
    // the bus in every registration. What it cannot do is produce a signature — that
    // needs the authenticator. What its leak costs is unlinkability: somebody could
    // tell that two registrations were the same device. §10.6 is where that sits
    // next to the rest of this device's custody story.
    const int written =
        snprintf(file_buffer, sizeof file_buffer,
                 "{\n"
                 "    \"v\": %d,\n"
                 "    \"credentialId\": \"%s\",\n"
                 "    \"publicKey\": \"%s\",\n"
                 "    \"userId\": \"%s\",\n"
                 "    \"keyHandle\": \"%s\",\n"
                 "    \"seedBlinding\": \"%s\",\n"
                 "    \"seedKem\": \"%s\",\n"
                 "    \"ikm\": \"%s\",\n"
                 "    \"ctx\": \"%s\",\n"
                 "    \"derivedPublicKey\": \"%s\",\n"
                 "    \"aaguid\": \"%s\",\n"
                 "    \"vendorId\": %u,\n"
                 "    \"productId\": %u\n"
                 "}\n",
                 kFormatVersion, credential_b64, public_b64, user_b64, handle_b64, blinding_b64,
                 kem_b64, ikm_b64, enrolment.ctx, derived_b64, enrolment.aaguid,
                 static_cast<unsigned>(enrolment.vendor_id),
                 static_cast<unsigned>(enrolment.product_id));
    if (written <= 0 || static_cast<size_t>(written) >= sizeof file_buffer) {
        return ESP_ERR_INVALID_SIZE;
    }
    return storage::WriteFileAtomically(kPath, kTempPath, file_buffer,
                                        static_cast<size_t>(written));
}

}  // namespace

const char *GateName(Gate gate) {
    switch (gate) {
        case Gate::kApproved:
            return "approved";
        case Gate::kNoKey:
            return "no-key";
        case Gate::kNotEnrolled:
            return "not-enrolled";
        case Gate::kWrongKey:
            return "wrong-key";
        case Gate::kDeclined:
            return "declined";
        case Gate::kTimeout:
            return "timeout";
        case Gate::kCancelled:
            return "cancelled";
        case Gate::kBadSignature:
            return "bad-signature";
        case Gate::kNoSignature:
            return "no-signature";
        case Gate::kTransport:
            return "transport";
    }
    return "?";
}

const char *GateText(Gate gate) {
    switch (gate) {
        case Gate::kApproved:
            return "the key confirmed this request";
        case Gate::kNoKey:
            return "no key on the OTG port";
        case Gate::kNotEnrolled:
            return "this device has no key enrolled - run `keyenrol`";
        case Gate::kWrongKey:
            return "this key does not hold this device's credential";
        case Gate::kDeclined:
            return "the key refused the request";
        case Gate::kTimeout:
            return "nobody touched the key";
        case Gate::kCancelled:
            return "the request went away before the key was touched";
        case Gate::kBadSignature:
            return "the key answered, and the answer did not verify";
        case Gate::kNoSignature:
            return "the key answered without signing anything - is previewSign supported?";
        case Gate::kTransport:
            return "the key could not be reached";
    }
    return "?";
}

esp_err_t Init() {
    // **Idempotent, and called here because this is the first thing in this
    // firmware that needs PSA.** `esp-tls` initialises it too when a TLS session
    // is opened, and this device may never open one — a bus with no TLS (§10.3)
    // is the ordinary case. Calling it twice is defined to be harmless; not
    // calling it at all is a verification that fails for a reason nothing prints.
    const psa_status_t psa = psa_crypto_init();
    if (psa != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_crypto_init: %d - no assertion will verify", static_cast<int>(psa));
    }

    Sha256(reinterpret_cast<const uint8_t *>(ctap2::kRelyingPartyId),
           std::strlen(ctap2::kRelyingPartyId), runtime.rp_id_hash);

    const esp_err_t loaded = Load();
    if (loaded == ESP_OK) {
        ESP_LOGI(TAG, "enrolled on %04x:%04x (aaguid %s), credential %u bytes",
                 runtime.enrolment.vendor_id, runtime.enrolment.product_id,
                 runtime.enrolment.aaguid,
                 static_cast<unsigned>(runtime.enrolment.credential_id_length));
        // **The key this device answers as** (§10.2). Printed at boot because it is
        // what the handler's allowlist has to contain, and an operator comparing the
        // two by eye is how a stale registration gets found in seconds instead of by
        // watching approvals silently fail.
        ESP_LOGI(TAG, "signing key (p256): %s", runtime.public_key_b64);
    } else if (loaded == ESP_ERR_NOT_FOUND) {
        ESP_LOGI(TAG, "no %s; this device has no key enrolled yet", kPath);
    }

    const esp_err_t err = usb::Init();
    if (err != ESP_OK) {
        // §10.10: not fatal. A device that cannot host a key cannot approve, and
        // that is the safe direction.
        ESP_LOGE(TAG, "the USB host would not start (%s); no key can be used",
                 esp_err_to_name(err));
        return err;
    }
    runtime.ready = true;
    return ESP_OK;
}

bool Ready() { return runtime.ready; }

// **Enrolled means "could sign"**, and that is why the derivation is part of it: a
// file that loaded but no longer derives the registered key is not an enrolment this
// device can answer requests with, and §10.10 rule 5 says it must not take them out
// of the queue group at all.
bool Enrolled() { return runtime.enrolment.present && runtime.derivation_holds; }

const Enrolment &Current() { return runtime.enrolment; }

const char *PublicKeyBase64() { return Enrolled() ? runtime.public_key_b64 : ""; }

bool DerivationHolds() { return runtime.derivation_holds; }

bool SelfTest(char *detail, size_t detail_size) {
    // The two steps the host tier cannot reach: the ECDH inside the KEM and the
    // point addition that blinds the key. Same seed key, same `ikm`, same `ctx` as
    // `tools/make_arkg_vectors.py` used — and if the answer differs, no security key
    // would ever have worked here.
    arkg::SeedKey seed;
    std::memcpy(seed.blinding, arkg::selftest::kBlindingPoint, sizeof seed.blinding);
    std::memcpy(seed.kem, arkg::selftest::kKemPoint, sizeof seed.kem);

    arkg::Derived derived;
    const arkg::Status status = arkg::Derive(
        arkg::PsaBackend(), seed, arkg::selftest::kIkm, sizeof arkg::selftest::kIkm,
        reinterpret_cast<const uint8_t *>(arkg::selftest::kCtx),
        std::strlen(arkg::selftest::kCtx), &derived);
    if (status != arkg::Status::kOk) {
        snprintf(detail, detail_size, "the derivation refused: %s", arkg::StatusName(status));
        return false;
    }

    if (std::memcmp(derived.compressed, arkg::selftest::kDerivedCompressed,
                    sizeof derived.compressed) != 0) {
        char produced[48] = {};
        crypto::Base64Encode(derived.compressed, sizeof derived.compressed, produced,
                             sizeof produced);
        snprintf(detail, detail_size, "wrong key: got %s, wanted %s", produced,
                 arkg::selftest::kDerivedPublicKeyB64);
        return false;
    }
    // The handle too. A device that derived the right public key with the wrong
    // handle would fail on the first real approval and nowhere earlier.
    if (std::memcmp(derived.key_handle, arkg::selftest::kKeyHandle,
                    sizeof derived.key_handle) != 0) {
        snprintf(detail, detail_size, "the public key matched and the key handle did not");
        return false;
    }

    snprintf(detail, detail_size, "the curve agrees with Python: %s",
             arkg::selftest::kDerivedPublicKeyB64);
    return true;
}

bool Present() { return usb::Present(); }

usb::DeviceInfo Device() { return usb::Device(); }

Stats GetStats() { return runtime.stats; }

esp_err_t Info(ctap2::Info *out, usb::Fault *fault, uint8_t *status) {
    size_t request_length = 0;
    if (!ctap2::BuildGetInfo(request_buffer, sizeof(request_buffer), &request_length)) {
        return ESP_ERR_INVALID_SIZE;
    }
    size_t response_length = 0;
    const esp_err_t err = usb::Exchange(ctaphid::kCmdCbor, request_buffer, request_length,
                                        response_buffer, sizeof(response_buffer),
                                        &response_length, 2000, nullptr, nullptr, fault, nullptr);
    if (err != ESP_OK) {
        return err;
    }
    if (!ctap2::ParseInfo(response_buffer, response_length, out, status)) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

esp_err_t Enrol(uint32_t timeout_ms, usb::Fault *fault, uint8_t *status) {
    if (!usb::Present()) {
        if (fault != nullptr) {
            *fault = usb::Fault::kNoDevice;
        }
        return ESP_ERR_NOT_FOUND;
    }

    Enrolment fresh;
    esp_fill_random(fresh.user_id, sizeof(fresh.user_id));

    // **The derivation entropy, and what it is and is not protecting.** `ikm` makes
    // this device's key unique and unlinkable; it is *not* a signing secret — the
    // private half is reconstructed inside the authenticator and nothing here can
    // produce one. So the RNG caveat of §10.7 applies more gently than it does to a
    // nonce: with the radio down this is a PRNG, and what a predictable value would
    // cost is unlinkability rather than a forgeable verdict. Enrolment happens from
    // the console on a device that is normally already on the network, so in
    // practice the radio is up.
    esp_fill_random(fresh.ikm, sizeof(fresh.ikm));
    snprintf(fresh.ctx, sizeof(fresh.ctx), "%s", arkg::kDeviceCtx);

    // **A random challenge, and it is not bound to anything.** Enrolment is not an
    // approval: nothing is being authorised, so there is nothing for the hash to
    // commit to. What matters is that it is unpredictable, so a `makeCredential`
    // response cannot be replayed from a capture.
    uint8_t challenge[ctap2::kClientDataHashSize];
    esp_fill_random(challenge, sizeof(challenge));

    size_t request_length = 0;
    if (!ctap2::BuildMakeCredentialArkg(challenge, fresh.user_id, request_buffer,
                                        sizeof(request_buffer), &request_length)) {
        return ESP_ERR_INVALID_SIZE;
    }

    size_t response_length = 0;
    const esp_err_t err =
        usb::Exchange(ctaphid::kCmdCbor, request_buffer, request_length, response_buffer,
                      sizeof(response_buffer), &response_length, timeout_ms, nullptr, nullptr,
                      fault, nullptr);
    if (err != ESP_OK) {
        return err;
    }

    // --- the credential: what will prove presence -------------------------
    ctap2::Credential credential;
    if (!ctap2::ParseMakeCredential(response_buffer, response_length, &credential, status)) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (credential.id_length > sizeof(fresh.credential_id)) {
        ESP_LOGE(TAG, "this key's credential id is %u bytes; the ceiling here is %u",
                 static_cast<unsigned>(credential.id_length),
                 static_cast<unsigned>(sizeof(fresh.credential_id)));
        return ESP_ERR_INVALID_SIZE;
    }
    if (!ctap2::CoseToP256Point(credential.cose_key, credential.cose_key_length, fresh.public_key,
                                sizeof(fresh.public_key))) {
        ESP_LOGE(TAG, "the key returned a public key this device cannot verify against");
        return ESP_ERR_INVALID_RESPONSE;
    }
    std::memcpy(fresh.credential_id, credential.id, credential.id_length);
    fresh.credential_id_length = credential.id_length;
    if (credential.aaguid != nullptr) {
        HexEncode(credential.aaguid, 16, fresh.aaguid);
    }

    // --- the generated key: what will make the verdict --------------------
    //
    // **A key that answered without generating one is not an enrolment.** It would
    // leave this device with a credential it can assert with and nothing to sign
    // with, which is exactly the state that looks like it worked. The most likely
    // cause is a key without `previewSign`, so the message says that rather than
    // reporting a parse failure.
    ctap2::GeneratedKey generated;
    if (!ctap2::ParseGeneratedKey(response_buffer, response_length, &generated, status)) {
        ESP_LOGE(TAG,
                 "this key made a credential and generated no signing key - it probably does "
                 "not support previewSign (`key info` lists what it does)");
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (generated.key_handle_length > sizeof(fresh.key_handle)) {
        ESP_LOGE(TAG, "this key's generated key handle is %u bytes; the ceiling here is %u",
                 static_cast<unsigned>(generated.key_handle_length),
                 static_cast<unsigned>(sizeof(fresh.key_handle)));
        return ESP_ERR_INVALID_SIZE;
    }
    if (!ctap2::ParseArkgSeedKey(generated.seed_cose_key, generated.seed_cose_key_length,
                                 fresh.seed.blinding, fresh.seed.kem)) {
        ESP_LOGE(TAG, "the generated key is not an ARKG-P256 seed key this firmware can read");
        return ESP_ERR_INVALID_RESPONSE;
    }
    std::memcpy(fresh.key_handle, generated.key_handle, generated.key_handle_length);
    fresh.key_handle_length = generated.key_handle_length;

    // --- and the derivation, here rather than at first use ----------------
    const arkg::Status derived = DeriveInto(&fresh);
    if (derived != arkg::Status::kOk) {
        ESP_LOGE(TAG, "the derivation refused (%s); nothing was changed",
                 arkg::StatusName(derived));
        return ESP_FAIL;
    }

    const usb::DeviceInfo device = usb::Device();
    fresh.vendor_id = device.vendor_id;
    fresh.product_id = device.product_id;
    fresh.present = true;

    // **Written before it is adopted.** A save that fails leaves the previous
    // enrolment in memory *and* on disk, which is the same ordering
    // `registration.json` uses: a failed attempt must not cost a working setup.
    const esp_err_t saved = Save(fresh);
    if (saved != ESP_OK) {
        ESP_LOGE(TAG, "the enrolment would not save (%s); nothing was changed",
                 esp_err_to_name(saved));
        return saved;
    }

    char encoded[48] = {};
    if (!crypto::Base64Encode(fresh.derived.compressed, sizeof(fresh.derived.compressed), encoded,
                              sizeof(encoded))) {
        return ESP_ERR_INVALID_SIZE;
    }
    runtime.enrolment = fresh;
    runtime.derivation_holds = true;
    snprintf(runtime.public_key_b64, sizeof(runtime.public_key_b64), "%s", encoded);
    runtime.stats.enrolments++;

    ESP_LOGI(TAG, "enrolled on %04x:%04x, credential %u bytes, key handle %u bytes",
             fresh.vendor_id, fresh.product_id,
             static_cast<unsigned>(fresh.credential_id_length),
             static_cast<unsigned>(fresh.key_handle_length));
    // **The two lines an operator has to act on.** The key is new, so the allowlist
    // entry naming the old one is worthless: this device cannot be verified until it
    // registers again with a fresh token (§6, §10.18.1). Said loudly here because the
    // alternative is a device that looks enrolled and is refused on every approval.
    ESP_LOGI(TAG, "signing key (p256): %s", runtime.public_key_b64);
    ESP_LOGW(TAG, "this is a NEW key - the registration is stale, run `register <token>` again");
    return ESP_OK;
}


esp_err_t Forget() {
    const esp_err_t err = storage::Remove(kPath);
    if (err != ESP_OK) {
        return err;
    }
    runtime.enrolment = Enrolment{};
    runtime.derivation_holds = false;
    runtime.public_key_b64[0] = 0;
    ESP_LOGW(TAG, "%s deleted; the credential is still on the key and cannot be removed from here",
             kPath);
    // **And the registration is now naming a key nothing holds.** Forgetting the
    // enrolment costs the ability to sign, so the allowlist entry is dead weight
    // until this device enrols again and registers with a fresh token (§10.18.1).
    ESP_LOGW(TAG, "this device can no longer sign anything - `key enrol`, then `register <token>`");
    return ESP_OK;
}

Gate Sign(const uint8_t *digest, uint32_t timeout_ms, usb::KeepAlive keep_alive,
          void *keep_alive_context, uint8_t *signature, size_t signature_capacity,
          size_t *signature_length, usb::Fault *fault, uint8_t *status) {
    usb::Fault local_fault = usb::Fault::kNone;
    uint8_t local_status = 0;
    if (fault == nullptr) {
        fault = &local_fault;
    }
    if (status == nullptr) {
        status = &local_status;
    }
    *fault = usb::Fault::kNone;
    *status = 0;
    if (signature_length != nullptr) {
        *signature_length = 0;
    }

    runtime.stats.asked++;

    if (digest == nullptr || signature == nullptr || signature_length == nullptr ||
        signature_capacity < ctap2::kMaxSignatureSize) {
        return Gate::kBadSignature;
    }
    if (!usb::Present()) {
        *fault = usb::Fault::kNoDevice;
        return Gate::kNoKey;
    }
    if (!Enrolled()) {
        return Gate::kNotEnrolled;
    }

    // COSE_Sign_Args, rebuilt per approval from the key handle the derivation
    // produced and the label it was scoped to. Cheap, and one fewer blob in
    // `fido.json` that could disagree with the rest of it.
    uint8_t args[ctap2::kMaxSignArgsSize];
    size_t args_length = 0;
    if (!ctap2::BuildSignArgs(runtime.enrolment.derived.key_handle,
                              sizeof(runtime.enrolment.derived.key_handle), runtime.enrolment.ctx,
                              std::strlen(runtime.enrolment.ctx), args, sizeof(args),
                              &args_length)) {
        return Gate::kBadSignature;
    }

    // **The same digest twice, and that is the design.** As `clientDataHash` it
    // makes the key's own signature commit to this request; as the extension's
    // `tbs` it is what the verdict's signature is over. One touch, two signatures,
    // one request — and neither can be lifted onto another one.
    size_t request_length = 0;
    if (!ctap2::BuildGetAssertionSign(digest, runtime.enrolment.credential_id,
                                      runtime.enrolment.credential_id_length,
                                      runtime.enrolment.key_handle,
                                      runtime.enrolment.key_handle_length, digest, args,
                                      args_length, request_buffer, sizeof(request_buffer),
                                      &request_length)) {
        return Gate::kBadSignature;
    }

    size_t response_length = 0;
    const esp_err_t err = usb::Exchange(ctaphid::kCmdCbor, request_buffer, request_length,
                                        response_buffer, sizeof(response_buffer),
                                        &response_length, timeout_ms, keep_alive,
                                        keep_alive_context, fault, nullptr);
    if (err != ESP_OK) {
        switch (*fault) {
            case usb::Fault::kTimeout:
                runtime.stats.timed_out++;
                return Gate::kTimeout;
            case usb::Fault::kCancelled:
                return Gate::kCancelled;
            case usb::Fault::kNoDevice:
            case usb::Fault::kUnplugged:
                return Gate::kNoKey;
            default:
                return Gate::kTransport;
        }
    }

    ctap2::Assertion assertion;
    if (!ctap2::ParseAssertion(response_buffer, response_length, &assertion, status)) {
        if (*status == ctap2::kErrNoCredentials) {
            runtime.stats.wrong_key++;
            return Gate::kWrongKey;
        }
        if (*status == ctap2::kErrActionTimeout || *status == ctap2::kErrUserActionPending) {
            runtime.stats.timed_out++;
            return Gate::kTimeout;
        }
        if (*status == ctap2::kErrKeepaliveCancel) {
            return Gate::kCancelled;
        }
        if (*status != ctap2::kOk) {
            return Gate::kDeclined;
        }
        return Gate::kBadSignature;
    }

    // --- Five checks, and all five have to pass ----------------------------
    //
    // Failing any of them lands on `kBadSignature` — deliberately the loudest
    // outcome this function has. A key that answers with something that does not
    // check out is either the wrong key, a bug in this firmware, or something
    // between the two pretending to be a key, and none of those is a reason to
    // publish anything.

    ctap2::AuthData auth;
    if (!ctap2::ParseAuthData(assertion.auth_data, assertion.auth_data_length, &auth)) {
        runtime.stats.bad_signature++;
        return Gate::kBadSignature;
    }

    // 1. The assertion is for *this* relying party. A key will not normally answer
    //    for another one, but the hash is in the signed bytes and checking it costs
    //    a memcmp.
    if (std::memcmp(auth.rp_id_hash, runtime.rp_id_hash, ctap2::kRpIdHashSize) != 0) {
        ESP_LOGE(TAG, "the assertion is for another relying party");
        runtime.stats.bad_signature++;
        return Gate::kBadSignature;
    }

    // 2. **Somebody touched it.** This is the bit the whole design rests on: a key
    //    that answered without user presence answered by itself, and an approval
    //    nobody made is the one outcome §10.10 exists to prevent. It survives the
    //    extension — the signature the extension carries is worth nothing without
    //    this flag inside the bytes the key signed.
    if ((auth.flags & ctap2::kFlagUserPresent) == 0) {
        ESP_LOGE(TAG, "the key answered without user presence");
        runtime.stats.bad_signature++;
        return Gate::kBadSignature;
    }

    // 3. It is the credential this device enrolled — when the key bothered to say
    //    which. With a one-entry allow list many keys omit the field, so an absent
    //    descriptor is not a failure; a *different* one is.
    if (assertion.credential_id != nullptr &&
        (assertion.credential_id_length != runtime.enrolment.credential_id_length ||
         std::memcmp(assertion.credential_id, runtime.enrolment.credential_id,
                     runtime.enrolment.credential_id_length) != 0)) {
        runtime.stats.wrong_key++;
        return Gate::kWrongKey;
    }

    // 4. The assertion's own signature, over `authData || digest`, against the
    //    credential's public key. Everything above this line is shape; this line is
    //    what makes the presence flag evidence rather than a claim.
    if (!VerifyAssertion(assertion.auth_data, assertion.auth_data_length, digest,
                         assertion.signature, assertion.signature_length)) {
        ESP_LOGE(TAG, "the assertion did not verify against the enrolled public key");
        runtime.stats.bad_signature++;
        return Gate::kBadSignature;
    }

    // 5. And the verdict's signature, which is the reason for all of the above.
    //
    //    Two ways this fails and they are different facts about the key: it can be
    //    **absent**, which means the key ignored the extension (almost always a key
    //    without `previewSign`, and `kNoSignature` says so), or it can be **there
    //    and wrong**, which means the derived key this device registered is not the
    //    one the authenticator reconstructed. The second is `kBadSignature`, and
    //    catching it here rather than letting the hook catch it is the difference
    //    between one loud log line and an approval that silently never lands.
    const uint8_t *verdict = nullptr;
    size_t verdict_length = 0;
    if (!ctap2::ParseSignSignature(assertion.auth_data, assertion.auth_data_length, &verdict,
                                   &verdict_length)) {
        ESP_LOGE(TAG, "the key did not sign the verdict - previewSign missing from its answer");
        return Gate::kNoSignature;
    }
    if (!VerifyP256(runtime.enrolment.derived.point, digest, verdict, verdict_length)) {
        ESP_LOGE(TAG,
                 "the key signed with a private key that does not match the registered public "
                 "one - the hook would reject this reply, so it is not sent");
        runtime.stats.bad_signature++;
        return Gate::kBadSignature;
    }

    std::memcpy(signature, verdict, verdict_length);
    *signature_length = verdict_length;
    runtime.stats.approved++;
    return Gate::kApproved;
}


}  // namespace fido
