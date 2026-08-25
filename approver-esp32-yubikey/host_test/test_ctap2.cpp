// CTAP2 requests and responses (CLAUDE.md §10.18.2).
//
// Two halves, and the second one is where the security is:
//
//   * **the requests** have to be bytes a real key accepts — canonical CBOR, the
//     right keys in the right order, and `up: true` present. A request the key
//     rejects is a device that never approves anything, and the error it answers
//     with does not name the cause;
//   * **the responses** arrive from a device on the other end of a cable, and
//     the parser is what stands between them and a verifier. Everything here is
//     built by hand out of bytes rather than round-tripped, because a round trip
//     through one implementation proves only that it is self-consistent.
//
// What this file does **not** test is the verification itself — that needs PSA
// Crypto and lives on the device tier (§10.11). What it does test is that a
// malformed answer never reaches the verifier at all.

#include <cstring>

#include "ctap2.h"
#include "unity.h"

namespace {

uint8_t buffer[512];

// --- Requests ------------------------------------------------------------

void test_ctap2_getinfo_is_one_byte(void) {
    size_t length = 0;
    TEST_ASSERT_TRUE(ctap2::BuildGetInfo(buffer, sizeof(buffer), &length));
    TEST_ASSERT_EQUAL_UINT32(1, length);
    TEST_ASSERT_EQUAL_UINT8(ctap2::kGetInfo, buffer[0]);
}

void test_ctap2_getassertion_starts_with_the_command_and_a_four_key_map(void) {
    uint8_t challenge[32];
    std::memset(challenge, 0xA5, sizeof(challenge));
    const uint8_t credential[] = {1, 2, 3, 4};

    size_t length = 0;
    TEST_ASSERT_TRUE(ctap2::BuildGetAssertion(challenge, credential, sizeof(credential), buffer,
                                              sizeof(buffer), &length));
    TEST_ASSERT_EQUAL_UINT8(ctap2::kGetAssertion, buffer[0]);
    TEST_ASSERT_EQUAL_UINT8(0xA4, buffer[1]);  // map, four pairs
    // Key 1 is the rpId, and it must be the constant rather than anything a
    // setting could reach (§10.18).
    TEST_ASSERT_EQUAL_UINT8(0x01, buffer[2]);
}

void test_ctap2_getassertion_asks_for_user_presence_out_loud(void) {
    // **The single most important byte this firmware sends.** `up: true` is what
    // makes the key refuse to answer until somebody touches it, and it is the
    // whole reason this gate is worth having. It is the default, and it is
    // written anyway — a default that silently changed would be a device that
    // approved things by itself.
    uint8_t challenge[32];
    std::memset(challenge, 0x11, sizeof(challenge));
    const uint8_t credential[] = {9};

    size_t length = 0;
    TEST_ASSERT_TRUE(ctap2::BuildGetAssertion(challenge, credential, sizeof(credential), buffer,
                                              sizeof(buffer), &length));

    // {"up": true} is `A1 62 75 70 F5`.
    const uint8_t up[] = {0xA1, 0x62, 'u', 'p', 0xF5};
    bool found = false;
    for (size_t i = 0; i + sizeof(up) <= length; i++) {
        if (std::memcmp(buffer + i, up, sizeof(up)) == 0) {
            found = true;
            break;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(found, "getAssertion did not ask for user presence");
}

void test_ctap2_getassertion_carries_the_challenge_verbatim(void) {
    // The challenge is what binds a fingertip to *this* request. A builder that
    // hashed, truncated or reordered it would produce a touch that authorises
    // nothing in particular.
    uint8_t challenge[32];
    for (size_t i = 0; i < sizeof(challenge); i++) {
        challenge[i] = static_cast<uint8_t>(i + 1);
    }
    const uint8_t credential[] = {7, 7};

    size_t length = 0;
    TEST_ASSERT_TRUE(ctap2::BuildGetAssertion(challenge, credential, sizeof(credential), buffer,
                                              sizeof(buffer), &length));

    bool found = false;
    for (size_t i = 0; i + sizeof(challenge) <= length; i++) {
        if (std::memcmp(buffer + i, challenge, sizeof(challenge)) == 0) {
            found = true;
            break;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(found, "the challenge did not reach the request intact");
}

void test_ctap2_getassertion_carries_the_credential_in_the_allow_list(void) {
    uint8_t challenge[32] = {};
    const uint8_t credential[] = {0xCA, 0xFE, 0xBA, 0xBE};
    size_t length = 0;
    TEST_ASSERT_TRUE(ctap2::BuildGetAssertion(challenge, credential, sizeof(credential), buffer,
                                              sizeof(buffer), &length));

    bool found = false;
    for (size_t i = 0; i + sizeof(credential) <= length; i++) {
        if (std::memcmp(buffer + i, credential, sizeof(credential)) == 0) {
            found = true;
            break;
        }
    }
    TEST_ASSERT_TRUE(found);
}

void test_ctap2_getassertion_refuses_a_missing_credential(void) {
    uint8_t challenge[32] = {};
    size_t length = 0;
    TEST_ASSERT_FALSE(
        ctap2::BuildGetAssertion(challenge, nullptr, 0, buffer, sizeof(buffer), &length));
    const uint8_t credential[] = {1};
    TEST_ASSERT_FALSE(
        ctap2::BuildGetAssertion(nullptr, credential, sizeof(credential), buffer,
                                 sizeof(buffer), &length));
}

void test_ctap2_a_request_that_does_not_fit_is_refused_rather_than_cut(void) {
    uint8_t challenge[32] = {};
    const uint8_t credential[] = {1, 2, 3};
    size_t length = 0;
    uint8_t tiny[8];
    TEST_ASSERT_FALSE(ctap2::BuildGetAssertion(challenge, credential, sizeof(credential), tiny,
                                               sizeof(tiny), &length));
}

void test_ctap2_makecredential_asks_for_es256(void) {
    uint8_t challenge[32] = {};
    uint8_t user[ctap2::kUserIdSize] = {};
    size_t length = 0;
    TEST_ASSERT_TRUE(
        ctap2::BuildMakeCredential(challenge, user, buffer, sizeof(buffer), &length));
    TEST_ASSERT_EQUAL_UINT8(ctap2::kMakeCredential, buffer[0]);
    TEST_ASSERT_EQUAL_UINT8(0xA4, buffer[1]);  // map, four pairs

    // `"alg": -7` is `63 61 6C 67 26`.
    const uint8_t alg[] = {0x63, 'a', 'l', 'g', 0x26};
    bool found = false;
    for (size_t i = 0; i + sizeof(alg) <= length; i++) {
        if (std::memcmp(buffer + i, alg, sizeof(alg)) == 0) {
            found = true;
            break;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(found, "makeCredential did not ask for ES256");
}

// --- Responses -----------------------------------------------------------

void test_ctap2_a_non_zero_status_is_a_failure_with_the_code_kept(void) {
    // The status byte leads every response, and the caller has to see it: "no
    // credential on this key" and "the key timed out" are two different
    // sentences for an operator.
    const uint8_t response[] = {ctap2::kErrNoCredentials};
    ctap2::Assertion assertion;
    uint8_t status = 0;
    TEST_ASSERT_FALSE(ctap2::ParseAssertion(response, sizeof(response), &assertion, &status));
    TEST_ASSERT_EQUAL_UINT8(ctap2::kErrNoCredentials, status);
    TEST_ASSERT_NOT_NULL(ctap2::StatusName(status));
}

void test_ctap2_an_empty_response_is_a_failure(void) {
    ctap2::Assertion assertion;
    uint8_t status = 0;
    TEST_ASSERT_FALSE(ctap2::ParseAssertion(nullptr, 0, &assertion, &status));
}

// Authenticator data with no attested credential: rpIdHash(32) flags(1) count(4).
void MakeAuthData(uint8_t *out, uint8_t flags) {
    std::memset(out, 0x33, ctap2::kRpIdHashSize);
    out[ctap2::kRpIdHashSize] = flags;
    out[ctap2::kRpIdHashSize + 1] = 0;
    out[ctap2::kRpIdHashSize + 2] = 0;
    out[ctap2::kRpIdHashSize + 3] = 0;
    out[ctap2::kRpIdHashSize + 4] = 5;
}

void test_ctap2_authdata_reads_the_flags_and_the_counter(void) {
    uint8_t auth[ctap2::kAuthDataMinSize];
    MakeAuthData(auth, ctap2::kFlagUserPresent);

    ctap2::AuthData parsed;
    TEST_ASSERT_TRUE(ctap2::ParseAuthData(auth, sizeof(auth), &parsed));
    TEST_ASSERT_EQUAL_UINT8(ctap2::kFlagUserPresent, parsed.flags);
    TEST_ASSERT_EQUAL_UINT32(5, parsed.sign_count);
    TEST_ASSERT_EQUAL_UINT8(0x33, parsed.rp_id_hash[0]);
    // No attested data flag, so no credential is claimed.
    TEST_ASSERT_NULL(parsed.credential_id);
}

void test_ctap2_authdata_shorter_than_its_fixed_part_is_refused(void) {
    uint8_t auth[ctap2::kAuthDataMinSize];
    MakeAuthData(auth, 0);
    ctap2::AuthData parsed;
    TEST_ASSERT_FALSE(ctap2::ParseAuthData(auth, ctap2::kAuthDataMinSize - 1, &parsed));
    TEST_ASSERT_FALSE(ctap2::ParseAuthData(nullptr, sizeof(auth), &parsed));
}

void test_ctap2_authdata_with_a_lying_credential_length_is_refused(void) {
    // The credential id length is two bytes the key chose. A parser that trusted
    // it would hand a `memcpy` a length that runs off the end of the response.
    uint8_t auth[ctap2::kAuthDataMinSize + 18 + 4];
    MakeAuthData(auth, ctap2::kFlagUserPresent | ctap2::kFlagAttestedData);
    size_t at = ctap2::kAuthDataMinSize;
    std::memset(auth + at, 0x00, 16);  // aaguid
    at += 16;
    auth[at] = 0x10;  // 4096 bytes of credential id, in a 55-byte buffer
    auth[at + 1] = 0x00;

    ctap2::AuthData parsed;
    TEST_ASSERT_FALSE(ctap2::ParseAuthData(auth, sizeof(auth), &parsed));
}

void test_ctap2_an_assertion_needs_both_authdata_and_a_signature(void) {
    // A response with one of the two is not an assertion, and must not reach a
    // verifier that would then be handed a null pointer.
    // {1: {...}, 2: h'...'} — no key 3.
    uint8_t auth[ctap2::kAuthDataMinSize];
    MakeAuthData(auth, ctap2::kFlagUserPresent);

    uint8_t response[128];
    size_t at = 0;
    response[at++] = ctap2::kOk;
    response[at++] = 0xA1;  // map, one pair
    response[at++] = 0x02;  // key 2
    response[at++] = 0x58;  // byte string, one-byte length
    response[at++] = static_cast<uint8_t>(sizeof(auth));
    std::memcpy(response + at, auth, sizeof(auth));
    at += sizeof(auth);

    ctap2::Assertion assertion;
    uint8_t status = 0;
    TEST_ASSERT_FALSE(ctap2::ParseAssertion(response, at, &assertion, &status));
    TEST_ASSERT_EQUAL_UINT8(ctap2::kOk, status);
}

void test_ctap2_a_well_formed_assertion_is_read(void) {
    uint8_t auth[ctap2::kAuthDataMinSize];
    MakeAuthData(auth, ctap2::kFlagUserPresent);
    const uint8_t signature[] = {0x30, 0x06, 0x02, 0x01, 0x01, 0x02, 0x01, 0x02};

    uint8_t response[128];
    size_t at = 0;
    response[at++] = ctap2::kOk;
    response[at++] = 0xA2;  // map, two pairs
    response[at++] = 0x02;
    response[at++] = 0x58;
    response[at++] = static_cast<uint8_t>(sizeof(auth));
    std::memcpy(response + at, auth, sizeof(auth));
    at += sizeof(auth);
    response[at++] = 0x03;
    response[at++] = static_cast<uint8_t>(0x40 | sizeof(signature));
    std::memcpy(response + at, signature, sizeof(signature));
    at += sizeof(signature);

    ctap2::Assertion assertion;
    uint8_t status = 0;
    TEST_ASSERT_TRUE(ctap2::ParseAssertion(response, at, &assertion, &status));
    TEST_ASSERT_EQUAL_UINT32(sizeof(auth), assertion.auth_data_length);
    TEST_ASSERT_EQUAL_UINT32(sizeof(signature), assertion.signature_length);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(signature, assertion.signature, sizeof(signature));

    // And the flag the whole gate rests on survives the trip.
    ctap2::AuthData parsed;
    TEST_ASSERT_TRUE(
        ctap2::ParseAuthData(assertion.auth_data, assertion.auth_data_length, &parsed));
    TEST_ASSERT_TRUE((parsed.flags & ctap2::kFlagUserPresent) != 0);
}

// --- The COSE key --------------------------------------------------------

// {1: 2, 3: -7, -1: 1, -2: h'x32', -3: h'y32'}
size_t MakeCoseKey(uint8_t *out, int64_t kty, int64_t alg, int64_t crv, size_t x_length,
                   size_t y_length) {
    size_t at = 0;
    out[at++] = 0xA5;
    out[at++] = 0x01;
    out[at++] = static_cast<uint8_t>(kty);
    out[at++] = 0x03;
    // alg is negative: major type 1, argument = -(alg) - 1
    out[at++] = static_cast<uint8_t>(0x20 | static_cast<uint8_t>(-alg - 1));
    out[at++] = 0x20;  // key -1
    out[at++] = static_cast<uint8_t>(crv);
    out[at++] = 0x21;  // key -2
    out[at++] = 0x58;
    out[at++] = static_cast<uint8_t>(x_length);
    std::memset(out + at, 0x01, x_length);
    at += x_length;
    out[at++] = 0x22;  // key -3
    out[at++] = 0x58;
    out[at++] = static_cast<uint8_t>(y_length);
    std::memset(out + at, 0x02, y_length);
    at += y_length;
    return at;
}

void test_ctap2_a_p256_cose_key_becomes_an_uncompressed_point(void) {
    uint8_t cose[128];
    const size_t length = MakeCoseKey(cose, 2, ctap2::kAlgEs256, 1, 32, 32);

    uint8_t point[ctap2::kP256PointSize];
    TEST_ASSERT_TRUE(ctap2::CoseToP256Point(cose, length, point, sizeof(point)));
    TEST_ASSERT_EQUAL_UINT8(0x04, point[0]);
    TEST_ASSERT_EQUAL_UINT8(0x01, point[1]);
    TEST_ASSERT_EQUAL_UINT8(0x02, point[33]);
}

void test_ctap2_a_cose_key_of_the_wrong_kind_is_refused(void) {
    // **Every one of these three checks is load-bearing.** A key that is not an
    // EC2 P-256 ES256 key must be refused here, before its coordinates reach a
    // verifier that would interpret them as if it were one.
    uint8_t cose[128];
    uint8_t point[ctap2::kP256PointSize];

    size_t length = MakeCoseKey(cose, 1, ctap2::kAlgEs256, 1, 32, 32);  // kty OKP
    TEST_ASSERT_FALSE(ctap2::CoseToP256Point(cose, length, point, sizeof(point)));

    length = MakeCoseKey(cose, 2, -8, 1, 32, 32);  // alg EdDSA
    TEST_ASSERT_FALSE(ctap2::CoseToP256Point(cose, length, point, sizeof(point)));

    length = MakeCoseKey(cose, 2, ctap2::kAlgEs256, 2, 32, 32);  // curve P-384
    TEST_ASSERT_FALSE(ctap2::CoseToP256Point(cose, length, point, sizeof(point)));
}

void test_ctap2_a_cose_key_with_short_coordinates_is_refused(void) {
    uint8_t cose[128];
    uint8_t point[ctap2::kP256PointSize];
    const size_t length = MakeCoseKey(cose, 2, ctap2::kAlgEs256, 1, 31, 32);
    TEST_ASSERT_FALSE(ctap2::CoseToP256Point(cose, length, point, sizeof(point)));
}

void test_ctap2_cose_refuses_a_buffer_that_cannot_hold_a_point(void) {
    uint8_t cose[128];
    const size_t length = MakeCoseKey(cose, 2, ctap2::kAlgEs256, 1, 32, 32);
    uint8_t small[ctap2::kP256PointSize - 1];
    TEST_ASSERT_FALSE(ctap2::CoseToP256Point(cose, length, small, sizeof(small)));
}

void test_ctap2_every_status_has_a_name(void) {
    TEST_ASSERT_NOT_NULL(ctap2::StatusName(ctap2::kOk));
    TEST_ASSERT_NOT_NULL(ctap2::StatusName(ctap2::kErrPinRequired));
    TEST_ASSERT_NOT_NULL(ctap2::StatusName(0xEE));
}

}  // namespace

void RegisterCtap2Tests(void) {
    RUN_TEST(test_ctap2_getinfo_is_one_byte);
    RUN_TEST(test_ctap2_getassertion_starts_with_the_command_and_a_four_key_map);
    RUN_TEST(test_ctap2_getassertion_asks_for_user_presence_out_loud);
    RUN_TEST(test_ctap2_getassertion_carries_the_challenge_verbatim);
    RUN_TEST(test_ctap2_getassertion_carries_the_credential_in_the_allow_list);
    RUN_TEST(test_ctap2_getassertion_refuses_a_missing_credential);
    RUN_TEST(test_ctap2_a_request_that_does_not_fit_is_refused_rather_than_cut);
    RUN_TEST(test_ctap2_makecredential_asks_for_es256);

    RUN_TEST(test_ctap2_a_non_zero_status_is_a_failure_with_the_code_kept);
    RUN_TEST(test_ctap2_an_empty_response_is_a_failure);
    RUN_TEST(test_ctap2_authdata_reads_the_flags_and_the_counter);
    RUN_TEST(test_ctap2_authdata_shorter_than_its_fixed_part_is_refused);
    RUN_TEST(test_ctap2_authdata_with_a_lying_credential_length_is_refused);
    RUN_TEST(test_ctap2_an_assertion_needs_both_authdata_and_a_signature);
    RUN_TEST(test_ctap2_a_well_formed_assertion_is_read);

    RUN_TEST(test_ctap2_a_p256_cose_key_becomes_an_uncompressed_point);
    RUN_TEST(test_ctap2_a_cose_key_of_the_wrong_kind_is_refused);
    RUN_TEST(test_ctap2_a_cose_key_with_short_coordinates_is_refused);
    RUN_TEST(test_ctap2_cose_refuses_a_buffer_that_cannot_hold_a_point);
    RUN_TEST(test_ctap2_every_status_has_a_name);
}
