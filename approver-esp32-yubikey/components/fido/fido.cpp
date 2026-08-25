// The gate (CLAUDE.md §10.18): enrolment on disk, one assertion per approval, and
// the verification that makes the assertion mean something.

#include "fido.h"

#include <cstdio>
#include <cstring>

#include "cJSON.h"
#include "cbor.h"
#include "ctaphid_frames.h"
#include "device_key.h"
#include "esp_log.h"
#include "esp_random.h"
#include "psa/crypto.h"
#include "storage.h"

namespace fido {
namespace {

constexpr const char *TAG = "fido";

// One request and one response buffer, static (§10.14.1). The largest request is
// a `makeCredential` — a client data hash, two small maps and one algorithm — and
// the largest response is a `getInfo`, which on a YubiKey 5 is around 400 bytes.
constexpr size_t kRequestMax = 512;
constexpr size_t kResponseMax = ctaphid::kMaxMessage;

uint8_t request_buffer[kRequestMax];
uint8_t response_buffer[kResponseMax];
char file_buffer[kMaxFileSize];

struct Runtime {
    bool ready = false;
    Enrolment enrolment;
    Stats stats;
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

// **The one function on this device that turns "a key answered" into "the key
// answered".** Everything above it is plumbing; this is the check.
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

    uint8_t raw_signature[64];
    if (!ParseDerSignature(signature, signature_length, raw_signature)) {
        return false;
    }

    // **The public key is imported for this one verification and destroyed
    // afterwards**, rather than kept as a PSA key across the life of the device.
    // It is a public key, so nothing is being protected by that — what it buys is
    // that there is no handle to leak, no slot to run out of, and no state that
    // can be stale after a `key enrol` replaces the enrolment underneath it.
    // Importing costs microseconds next to a fingertip.
    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attributes, PSA_KEY_TYPE_ECC_PUBLIC_KEY(PSA_ECC_FAMILY_SECP_R1));
    psa_set_key_bits(&attributes, 256);
    psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_VERIFY_HASH);
    psa_set_key_algorithm(&attributes, PSA_ALG_ECDSA(PSA_ALG_SHA_256));

    mbedtls_svc_key_id_t key = MBEDTLS_SVC_KEY_ID_INIT;
    if (psa_import_key(&attributes, runtime.enrolment.public_key, ctap2::kP256PointSize, &key) !=
        PSA_SUCCESS) {
        return false;
    }

    const psa_status_t verified = psa_verify_hash(key, PSA_ALG_ECDSA(PSA_ALG_SHA_256), digest,
                                                  sizeof(digest), raw_signature,
                                                  sizeof(raw_signature));
    psa_destroy_key(key);
    return verified == PSA_SUCCESS;
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

    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, "credentialId");
    if (cJSON_IsString(item) && item->valuestring != nullptr) {
        loaded.credential_id_length = crypto::Base64Decode(item->valuestring, loaded.credential_id,
                                                           sizeof(loaded.credential_id));
        good = good && loaded.credential_id_length > 0;
    } else {
        good = false;
    }

    item = cJSON_GetObjectItemCaseSensitive(root, "publicKey");
    if (cJSON_IsString(item) && item->valuestring != nullptr) {
        good = good && crypto::Base64Decode(item->valuestring, loaded.public_key,
                                            sizeof(loaded.public_key)) ==
                           ctap2::kP256PointSize;
        // The point has to be an uncompressed one or the verifier will not take
        // it. Checked at load rather than at the first approval, so that a
        // corrupted file is a boot log line and not a refused verdict.
        good = good && loaded.public_key[0] == 0x04;
    } else {
        good = false;
    }

    item = cJSON_GetObjectItemCaseSensitive(root, "userId");
    if (cJSON_IsString(item) && item->valuestring != nullptr) {
        good = good && crypto::Base64Decode(item->valuestring, loaded.user_id,
                                            sizeof(loaded.user_id)) == ctap2::kUserIdSize;
    }

    item = cJSON_GetObjectItemCaseSensitive(root, "aaguid");
    if (cJSON_IsString(item) && item->valuestring != nullptr) {
        snprintf(loaded.aaguid, sizeof(loaded.aaguid), "%s", item->valuestring);
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

    loaded.present = true;
    runtime.enrolment = loaded;
    return ESP_OK;
}

esp_err_t Save(const Enrolment &enrolment) {
    char credential_b64[ctap2::kMaxCredentialIdSize * 2 + 4];
    char public_b64[ctap2::kP256PointSize * 2 + 4];
    char user_b64[ctap2::kUserIdSize * 2 + 4];

    if (!crypto::Base64Encode(enrolment.credential_id, enrolment.credential_id_length,
                              credential_b64, sizeof(credential_b64)) ||
        !crypto::Base64Encode(enrolment.public_key, ctap2::kP256PointSize, public_b64,
                              sizeof(public_b64)) ||
        !crypto::Base64Encode(enrolment.user_id, ctap2::kUserIdSize, user_b64,
                              sizeof(user_b64))) {
        return ESP_ERR_INVALID_SIZE;
    }

    // Hand-written rather than through cJSON, because there are five fields and
    // none of them can contain a character that would need escaping — base64,
    // hex and two integers. The one thing that has to be right is that the file
    // is valid JSON, and a test asserts it round-trips.
    const int written = snprintf(file_buffer, sizeof(file_buffer),
                                 "{\n"
                                 "    \"credentialId\": \"%s\",\n"
                                 "    \"publicKey\": \"%s\",\n"
                                 "    \"userId\": \"%s\",\n"
                                 "    \"aaguid\": \"%s\",\n"
                                 "    \"vendorId\": %u,\n"
                                 "    \"productId\": %u\n"
                                 "}\n",
                                 credential_b64, public_b64, user_b64, enrolment.aaguid,
                                 static_cast<unsigned>(enrolment.vendor_id),
                                 static_cast<unsigned>(enrolment.product_id));
    if (written <= 0 || static_cast<size_t>(written) >= sizeof(file_buffer)) {
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

bool Enrolled() { return runtime.enrolment.present; }

const Enrolment &Current() { return runtime.enrolment; }

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

    // **A random challenge, and it is not bound to anything.** Enrolment is not
    // an approval: nothing is being authorised, so there is nothing for the hash
    // to commit to. What matters is that it is unpredictable, so that a
    // makeCredential response cannot be replayed from a capture.
    uint8_t challenge[ctap2::kClientDataHashSize];
    esp_fill_random(challenge, sizeof(challenge));

    size_t request_length = 0;
    if (!ctap2::BuildMakeCredential(challenge, fresh.user_id, request_buffer,
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
    runtime.enrolment = fresh;
    runtime.stats.enrolments++;
    // Nothing to notify: the responder re-asks `WhyNot` on its own tick twice a
    // second, so a `key enrol` puts this device on `approvals.*` within half a
    // second of the touch. Written down because "why does this not tell anybody"
    // is the obvious question, and the answer is that a poll already exists.
    ESP_LOGI(TAG, "enrolled on %04x:%04x, credential %u bytes", fresh.vendor_id, fresh.product_id,
             static_cast<unsigned>(fresh.credential_id_length));
    return ESP_OK;
}

esp_err_t Forget() {
    const esp_err_t err = storage::Remove(kPath);
    if (err != ESP_OK) {
        return err;
    }
    runtime.enrolment = Enrolment{};
    ESP_LOGW(TAG, "%s deleted; the credential is still on the key and cannot be removed from here",
             kPath);
    return ESP_OK;
}

Gate RequireTouch(const uint8_t *challenge, uint32_t timeout_ms, usb::KeepAlive keep_alive,
                  void *keep_alive_context, usb::Fault *fault, uint8_t *status) {
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

    runtime.stats.asked++;

    if (challenge == nullptr) {
        return Gate::kBadSignature;
    }
    if (!usb::Present()) {
        *fault = usb::Fault::kNoDevice;
        return Gate::kNoKey;
    }
    if (!runtime.enrolment.present) {
        return Gate::kNotEnrolled;
    }

    size_t request_length = 0;
    if (!ctap2::BuildGetAssertion(challenge, runtime.enrolment.credential_id,
                                  runtime.enrolment.credential_id_length, request_buffer,
                                  sizeof(request_buffer), &request_length)) {
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

    // --- Four checks, and all four have to pass -----------------------------
    //
    // Failing any of them lands on `kBadSignature`, which is deliberately the
    // loudest outcome this function has. A key that answers with something that
    // does not check out is either the wrong key, a bug in this firmware, or
    // something between the two pretending to be a key — and none of those is a
    // reason to sign anything.

    ctap2::AuthData auth;
    if (!ctap2::ParseAuthData(assertion.auth_data, assertion.auth_data_length, &auth)) {
        runtime.stats.bad_signature++;
        return Gate::kBadSignature;
    }

    // 1. The assertion is for *this* relying party. A key will not normally
    //    answer for another one, but the hash is in the signed bytes and
    //    checking it costs a memcmp.
    if (std::memcmp(auth.rp_id_hash, runtime.rp_id_hash, ctap2::kRpIdHashSize) != 0) {
        ESP_LOGE(TAG, "the assertion is for another relying party");
        runtime.stats.bad_signature++;
        return Gate::kBadSignature;
    }

    // 2. **Somebody touched it.** This is the bit the whole design rests on: a
    //    key that answered without user presence answered by itself, and an
    //    approval nobody made is the one outcome §10.10 exists to prevent.
    if ((auth.flags & ctap2::kFlagUserPresent) == 0) {
        ESP_LOGE(TAG, "the key answered without user presence");
        runtime.stats.bad_signature++;
        return Gate::kBadSignature;
    }

    // 3. It is the credential this device enrolled — when the key bothered to
    //    say which. With a one-entry allow list many keys omit the field, so an
    //    absent descriptor is not a failure; a *different* one is.
    if (assertion.credential_id != nullptr &&
        (assertion.credential_id_length != runtime.enrolment.credential_id_length ||
         std::memcmp(assertion.credential_id, runtime.enrolment.credential_id,
                     runtime.enrolment.credential_id_length) != 0)) {
        runtime.stats.wrong_key++;
        return Gate::kWrongKey;
    }

    // 4. And the signature over `authData || challenge`, against the public key
    //    stored at enrolment. Everything above this line is shape; this is the
    //    line that makes it evidence.
    if (!VerifyAssertion(assertion.auth_data, assertion.auth_data_length, challenge,
                         assertion.signature, assertion.signature_length)) {
        ESP_LOGE(TAG, "the assertion did not verify against the enrolled public key");
        runtime.stats.bad_signature++;
        return Gate::kBadSignature;
    }

    runtime.stats.approved++;
    return Gate::kApproved;
}

}  // namespace fido
