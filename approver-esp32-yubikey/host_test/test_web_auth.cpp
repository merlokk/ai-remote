// Who may reach the configuration site at all (CLAUDE.md §10.16), tested where
// it costs nothing (§10.11, host tier).
//
// `web/web_auth.h` includes `<cstddef>` and `<cstdint>` and nothing else — the
// shape every decision-holding file in this firmware has, and here the reason is
// the strongest one there is: this is the check that stands between a LAN and a
// device that can be pointed at another NATS server. A check that needs a board
// to exercise is a check nobody exercises.
//
// The suite reads as three sentences: **it is off until somebody configures it**
// (so a device flashed with the shipped `config.json` behaves exactly as it did),
// **a wrong credential is refused every way it can be wrong**, and **the encoder
// agrees with the rest of the world** — the last of those is what makes a browser
// able to log in at all.

#include "unity.h"
#include "web_auth.h"

#include <cstring>

namespace {

// The credentials used throughout, and the base64 a browser sends for them.
// Written out by hand rather than produced by the encoder under test, because a
// test that asks the encoder what it thinks is a test of nothing:
//
//   admin:secret  ->  YWRtaW46c2VjcmV0
constexpr const char *kUser = "admin";
constexpr const char *kPassword = "secret";
constexpr const char *kHeader = "Basic YWRtaW46c2VjcmV0";

}  // namespace

// --- Whether it is on at all ---------------------------------------------

void test_no_credentials_in_the_config_is_no_authentication(void) {
    // The shipped `config.json` has neither, and a device flashed with it serves
    // exactly what it served before this file existed. That is the whole of why
    // the switch is the presence of the pair rather than a third boolean.
    TEST_ASSERT_FALSE(web::AuthRequired("", ""));
    TEST_ASSERT_FALSE(web::AuthRequired(nullptr, nullptr));
}

void test_half_a_credential_is_not_authentication(void) {
    // **Both, or neither.** A user with no password would be a lock that opens to
    // an empty box, and a password with no user is a credential nobody can send.
    // Either half alone leaves the site open — and it says so in the readout,
    // which is the part that keeps this from being a silent misconfiguration.
    TEST_ASSERT_FALSE(web::AuthRequired(kUser, ""));
    TEST_ASSERT_FALSE(web::AuthRequired("", kPassword));
    TEST_ASSERT_FALSE(web::AuthRequired(kUser, nullptr));
    TEST_ASSERT_FALSE(web::AuthRequired(nullptr, kPassword));
}

void test_both_halves_switch_it_on(void) {
    TEST_ASSERT_TRUE(web::AuthRequired(kUser, kPassword));
}

// --- The gate ------------------------------------------------------------

void test_an_unconfigured_site_lets_everyone_in(void) {
    // `Authorised` is the whole gate rather than the second half of one, so no
    // handler can ask the questions in the wrong order. With nothing configured it
    // answers yes to a request that carries no header at all.
    TEST_ASSERT_TRUE(web::Authorised("", "", nullptr));
    TEST_ASSERT_TRUE(web::Authorised("", "", "Basic bm9uc2Vuc2U="));
}

void test_the_right_credential_gets_in(void) {
    TEST_ASSERT_TRUE(web::Authorised(kUser, kPassword, kHeader));
}

void test_no_header_at_all_is_refused(void) {
    // Which is what a browser's *first* request looks like: it has no idea a
    // credential is wanted until the 401 tells it.
    TEST_ASSERT_FALSE(web::Authorised(kUser, kPassword, nullptr));
    TEST_ASSERT_FALSE(web::Authorised(kUser, kPassword, ""));
}

void test_a_wrong_password_is_refused(void) {
    // admin:secrez
    TEST_ASSERT_FALSE(web::Authorised(kUser, kPassword, "Basic YWRtaW46c2VjcmV6"));
}

void test_a_wrong_user_is_refused(void) {
    // admon:secret
    TEST_ASSERT_FALSE(web::Authorised(kUser, kPassword, "Basic YWRtb246c2VjcmV0"));
}

void test_the_colon_cannot_be_moved(void) {
    // `admin:secret` and `admin:` + `secret` are the same bytes on the wire, so a
    // credential that puts the colon somewhere else must not open the same door:
    // `admi:nsecret` decodes to a different pair and encodes to different base64,
    // which is exactly what comparing the encoded form gets for free.
    TEST_ASSERT_FALSE(web::Authorised(kUser, kPassword, "Basic YWRtaTpuc2VjcmV0"));
}

void test_another_scheme_is_refused(void) {
    TEST_ASSERT_FALSE(web::Authorised(kUser, kPassword, "Bearer YWRtaW46c2VjcmV0"));
    TEST_ASSERT_FALSE(web::Authorised(kUser, kPassword, "Digest YWRtaW46c2VjcmV0"));
    // The credential on its own, with no scheme in front of it.
    TEST_ASSERT_FALSE(web::Authorised(kUser, kPassword, "YWRtaW46c2VjcmV0"));
    // And a scheme that merely *starts* like ours.
    TEST_ASSERT_FALSE(web::Authorised(kUser, kPassword, "BasicX YWRtaW46c2VjcmV0"));
}

void test_the_scheme_is_case_insensitive_and_the_credential_is_not(void) {
    // RFC 7235 says the scheme token is case-insensitive, and some clients take
    // it at its word. The base64 after it is data, and data is compared byte for
    // byte — `ywrtaw46c2vjcmv0` is not the credential, it is a different string.
    TEST_ASSERT_TRUE(web::Authorised(kUser, kPassword, "basic YWRtaW46c2VjcmV0"));
    TEST_ASSERT_TRUE(web::Authorised(kUser, kPassword, "BASIC YWRtaW46c2VjcmV0"));
    TEST_ASSERT_FALSE(web::Authorised(kUser, kPassword, "Basic ywrtaw46c2vjcmv0"));
}

void test_whitespace_around_the_credential_is_tolerated(void) {
    // More than one space after the scheme is legal HTTP, and a header with a
    // trailing space is what a hand-written client sends. Neither is a wrong
    // password, and answering 401 to one would be an afternoon somebody never
    // gets back.
    TEST_ASSERT_TRUE(web::Authorised(kUser, kPassword, "Basic   YWRtaW46c2VjcmV0"));
    TEST_ASSERT_TRUE(web::Authorised(kUser, kPassword, "Basic YWRtaW46c2VjcmV0  "));
    TEST_ASSERT_TRUE(web::Authorised(kUser, kPassword, "Basic\tYWRtaW46c2VjcmV0\r\n"));
}

void test_nothing_after_the_scheme_is_refused(void) {
    TEST_ASSERT_FALSE(web::Authorised(kUser, kPassword, "Basic"));
    TEST_ASSERT_FALSE(web::Authorised(kUser, kPassword, "Basic "));
    TEST_ASSERT_FALSE(web::Authorised(kUser, kPassword, "Basic     "));
}

void test_a_credential_that_only_starts_right_is_refused(void) {
    // The comparison runs to the end of the *expected* string, so a prefix must
    // not pass — this is the one mistake in a hand-written compare that a fuzzer
    // never finds and a person walks straight through.
    TEST_ASSERT_FALSE(web::Authorised(kUser, kPassword, "Basic YWRtaW46c2VjcmV"));
    TEST_ASSERT_FALSE(web::Authorised(kUser, kPassword, "Basic YWRtaW46c2VjcmV0x"));
}

// --- The encoder ---------------------------------------------------------

void test_the_encoder_agrees_with_the_rest_of_the_world(void) {
    // The three residues of base64, because the padding is where an encoder
    // written by hand goes wrong. Vectors from RFC 4648 §10 and one of our own.
    struct Case {
        const char *user;
        const char *password;
        const char *expected;
    };
    // "f:oobar" is 7 bytes (1 over), "fo:obar" is 7, "admin:secret" is 12 (0 over)
    const Case cases[] = {
        {"admin", "secret", "YWRtaW46c2VjcmV0"},  // 12 bytes, no padding
        {"a", "b", "YTpi"},                       // 3 bytes, no padding
        {"a", "bc", "YTpiYw=="},                  // 4 bytes, two pad
        {"a", "bcd", "YTpiY2Q="},                 // 5 bytes, one pad
        {"", "", "Og=="},                         // just the colon
    };
    for (const Case &item : cases) {
        char out[web::kMaxEncodedSize] = {};
        TEST_ASSERT_TRUE_MESSAGE(
            web::BasicCredential(item.user, item.password, out, sizeof out), item.expected);
        TEST_ASSERT_EQUAL_STRING(item.expected, out);
    }
}

void test_the_longest_credential_this_device_holds_still_encodes(void) {
    // The buffer is sized from `config::kWebUserSize` and `kWebPasswordSize`, and
    // this is the test that says so: 32 + 1 + 64 bytes has to fit, because a
    // credential that cannot be encoded is a device nobody can log into.
    char user[33] = {};
    char password[65] = {};
    std::memset(user, 'u', sizeof user - 1);
    std::memset(password, 'p', sizeof password - 1);

    char out[web::kMaxEncodedSize] = {};
    TEST_ASSERT_TRUE(web::BasicCredential(user, password, out, sizeof out));
    TEST_ASSERT_EQUAL(0, std::strlen(out) % 4);
}

void test_a_credential_too_long_to_encode_is_refused_not_truncated(void) {
    // **Nothing is written on a refusal**, the rule §10.2 keeps about the signing
    // bytes and `web_paths.h` about a file name: a truncated credential is one
    // that matches something nobody typed.
    char password[web::kMaxCredentialSize + 8] = {};
    std::memset(password, 'p', sizeof password - 1);

    char out[web::kMaxEncodedSize] = {'!', '\0'};
    TEST_ASSERT_FALSE(web::BasicCredential("admin", password, out, sizeof out));
    TEST_ASSERT_EQUAL_STRING("!", out);

    // And the gate refuses rather than letting anybody in, which is the
    // fail-closed half of the same sentence: a credential the device cannot
    // represent is not a credential that matches every header.
    TEST_ASSERT_FALSE(web::Authorised("admin", password, "Basic YWRtaW46c2VjcmV0"));
}

void test_a_buffer_too_small_is_refused(void) {
    char out[4] = {};
    TEST_ASSERT_FALSE(web::BasicCredential("admin", "secret", out, sizeof out));
    TEST_ASSERT_FALSE(web::BasicCredential("admin", "secret", nullptr, 64));
}

void test_a_colon_in_the_user_is_refused(void) {
    // Basic authentication cannot represent it: `a:b` + `c` and `a` + `b:c` are
    // the same bytes, so a user with a colon in it is two credentials wearing one
    // name. Refused where it is cheap rather than made ambiguous on the wire —
    // and refused *closed*, so a config with one in it locks the site rather than
    // opening it.
    char out[web::kMaxEncodedSize] = {};
    TEST_ASSERT_FALSE(web::BasicCredential("ad:min", "secret", out, sizeof out));
    TEST_ASSERT_TRUE(web::AuthRequired("ad:min", "secret"));
    TEST_ASSERT_FALSE(web::Authorised("ad:min", "secret", "Basic YWQ6bWluOnNlY3JldA=="));
}

void test_a_password_may_contain_a_colon(void) {
    // The other half of the same rule: everything after the first colon is the
    // password, so a colon in *it* is unambiguous and must work. `a:b:c` ->
    // user `a`, password `b:c`.
    char out[web::kMaxEncodedSize] = {};
    TEST_ASSERT_TRUE(web::BasicCredential("a", "b:c", out, sizeof out));
    TEST_ASSERT_EQUAL_STRING("YTpiOmM=", out);
    TEST_ASSERT_TRUE(web::Authorised("a", "b:c", "Basic YTpiOmM="));
}

void RegisterWebAuthTests(void) {
    RUN_TEST(test_no_credentials_in_the_config_is_no_authentication);
    RUN_TEST(test_half_a_credential_is_not_authentication);
    RUN_TEST(test_both_halves_switch_it_on);

    RUN_TEST(test_an_unconfigured_site_lets_everyone_in);
    RUN_TEST(test_the_right_credential_gets_in);
    RUN_TEST(test_no_header_at_all_is_refused);
    RUN_TEST(test_a_wrong_password_is_refused);
    RUN_TEST(test_a_wrong_user_is_refused);
    RUN_TEST(test_the_colon_cannot_be_moved);
    RUN_TEST(test_another_scheme_is_refused);
    RUN_TEST(test_the_scheme_is_case_insensitive_and_the_credential_is_not);
    RUN_TEST(test_whitespace_around_the_credential_is_tolerated);
    RUN_TEST(test_nothing_after_the_scheme_is_refused);
    RUN_TEST(test_a_credential_that_only_starts_right_is_refused);

    RUN_TEST(test_the_encoder_agrees_with_the_rest_of_the_world);
    RUN_TEST(test_the_longest_credential_this_device_holds_still_encodes);
    RUN_TEST(test_a_credential_too_long_to_encode_is_refused_not_truncated);
    RUN_TEST(test_a_buffer_too_small_is_refused);
    RUN_TEST(test_a_colon_in_the_user_is_refused);
    RUN_TEST(test_a_password_may_contain_a_colon);
}
