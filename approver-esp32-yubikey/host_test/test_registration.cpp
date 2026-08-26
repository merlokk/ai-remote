// The registration exchange (CLAUDE.md §10.11, host tier; §6, §10.7).
//
// **The suite is mostly about one rule**: a reply on `registrations` is not read
// until its signature has been checked. `registrations` is an open subject
// (§10.3), so anybody on the LAN can answer — an unsigned
// `{"ok":false,"error":"expired"}` would send an operator hunting a problem that
// does not exist, and an unsigned `ok:true` would be very much worse.
//
// That rule is testable here only because the signature check comes in as a
// function pointer (`registration.h` argues why). The verifier used below
// **records the message it was handed**, which turns "the bytes handed to the
// verifier are §6's signing bytes" from a reading of the code into an assertion —
// and the expected strings are Python's own output from
// `approver/protocol.py::registration_reply_signing_bytes`, pasted in, never
// derived from the header.
//
// The rest, in the order the file checks them: the version, the `ok` flag, the
// handler key and the **pin** on it, the nonce echo, the timestamp, the bounded
// strings, and the signature. Each has its own case, because each has its own
// sentence on a console and "the handler said no" and "somebody else said no" are
// completely different problems.

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "registration.h"
#include "unity.h"

using protocol::RegistrationReply;
using protocol::RegistrationRequest;
using protocol::ReplyStatus;

namespace {

constexpr char kNonce[] = "Tm9uY2VOb25jZU5vbmNlTm9uY2VOb25jZU5vbmNlMTI=";
constexpr char kServerKey[] = "A6EHv/POEL4dcN0Y50vAmWfk1jCbpQ1fHdyGZBJVMbg=";
constexpr char kOtherKey[] = "dFEabltdxgbWgh3CE5XL7ul7oMAAtc248oYeAKfJyzA=";
constexpr char kSig[] =
    "BAzU2LyejENvC1nkIPHSfBpKP30SSGPZpGfjpPzApJUEg3kkTQP10cKjFKD15gQjlTq19AilUqET4kvTkwG/CQ==";

// --- the verifier, and what it remembers -----------------------------------

struct Recorder {
    bool answer = true;
    int calls = 0;
    char public_key[128] = {};
    char signature[128] = {};
    char message[512] = {};
    size_t length = 0;
};

bool Record(const char *public_key_b64, const char *message, size_t length,
            const char *signature_b64, void *user) {
    Recorder *r = static_cast<Recorder *>(user);
    r->calls++;
    std::snprintf(r->public_key, sizeof r->public_key, "%s", public_key_b64);
    std::snprintf(r->signature, sizeof r->signature, "%s", signature_b64);
    r->length = length;
    if (length < sizeof r->message) {
        std::memcpy(r->message, message, length);
        r->message[length] = '\0';
    }
    return r->answer;
}

// A verifier that must never run. Every "the reply was rejected before the
// signature" case uses it, which is how the *ordering* is asserted rather than
// only the outcome.
bool NeverCalled(const char *, const char *, size_t, const char *, void *user) {
    *static_cast<bool *>(user) = true;
    return true;
}

ReplyStatus Parse(const char *json, Recorder *recorder, const char *pinned = nullptr,
                  RegistrationReply *out = nullptr) {
    RegistrationReply scratch;
    return protocol::ParseRegistrationReply(json, std::strlen(json), kNonce, pinned, &Record,
                                            recorder, out != nullptr ? out : &scratch);
}

void AssertRejectedBeforeVerifying(const char *json, ReplyStatus expected) {
    bool called = false;
    RegistrationReply out;
    const ReplyStatus status = protocol::ParseRegistrationReply(
        json, std::strlen(json), kNonce, nullptr, &NeverCalled, &called, &out);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(expected), static_cast<int>(status));
    TEST_ASSERT_FALSE_MESSAGE(called, "the signature was checked on a reply that is not one");
}

// ---------------------------------------------------------------------------
// The signing bytes moved to the parity vectors (§10.11 tier 2).
//
// They were three pasted literals here, and `test_vectors.cpp` now runs the same
// assembler over a **generated** set that covers the same three cases and three
// more — including an `error` with a separator inside it, which is the vector
// that makes "`error` is last because it is free text" load-bearing rather than
// decorative.
//
// What is still asserted below is the ordering, the refusals and the pin: the
// rules this implementation keeps, as opposed to the bytes both languages have
// to agree on.

// `true`/`false`, not `1`/`0`, and it is the difference between a reply that
// verifies and one that does not.
void test_the_ok_flag_is_spelled_the_way_json_spells_it(void) {
    char yes[protocol::kReplySigningBytesMax];
    char no[protocol::kReplySigningBytesMax];
    protocol::RegistrationReplySigningBytes(1, true, "k", "n", 0, "", yes, sizeof yes);
    protocol::RegistrationReplySigningBytes(1, false, "k", "n", 0, "", no, sizeof no);
    TEST_ASSERT_NOT_NULL(std::strstr(yes, "\ntrue\n"));
    TEST_ASSERT_NOT_NULL(std::strstr(no, "\nfalse\n"));
}

void test_reply_signing_bytes_refuse_what_does_not_fit(void) {
    char out[protocol::kReplySigningBytesMax];
    char oversized[protocol::kErrorMax + 8];
    std::memset(oversized, 'e', sizeof oversized);
    oversized[sizeof oversized - 1] = '\0';

    TEST_ASSERT_EQUAL_UINT32(
        0, protocol::RegistrationReplySigningBytes(1, true, "k", "n", 0, oversized, out, sizeof out));

    // And a buffer too small for a message that is otherwise fine.
    TEST_ASSERT_EQUAL_UINT32(0,
                             protocol::RegistrationReplySigningBytes(1, true, "k", "n", 0, "", out, 4));
}

// ---------------------------------------------------------------------------
// The token.

void test_a_token_splits_at_the_first_dot(void) {
    char key_id[protocol::kKeyIdMax + 1];

    TEST_ASSERT_TRUE(protocol::ParseToken("approver-esp32.c2VjcmV0", key_id, sizeof key_id));
    TEST_ASSERT_EQUAL_STRING("approver-esp32", key_id);

    // The secret is base64 and can contain a `.`? It cannot — but the handler
    // splits at the first dot, so this device must too, or the two disagree about
    // whose slot is being registered.
    TEST_ASSERT_TRUE(protocol::ParseToken("name.a.b", key_id, sizeof key_id));
    TEST_ASSERT_EQUAL_STRING("name", key_id);
}

void test_a_token_that_is_not_one_is_refused(void) {
    char key_id[protocol::kKeyIdMax + 1];
    TEST_ASSERT_FALSE(protocol::ParseToken("no-dot-here", key_id, sizeof key_id));
    TEST_ASSERT_FALSE(protocol::ParseToken(".secret", key_id, sizeof key_id));
    TEST_ASSERT_FALSE(protocol::ParseToken("", key_id, sizeof key_id));
    TEST_ASSERT_FALSE(protocol::ParseToken(nullptr, key_id, sizeof key_id));

    char tiny[4];
    TEST_ASSERT_FALSE(protocol::ParseToken("approver-esp32.x", tiny, sizeof tiny));
}

// ---------------------------------------------------------------------------
// The request.

void test_the_request_carries_what_six_asks_for(void) {
    RegistrationRequest request;
    request.ts = 1737345600;
    request.token = "approver-esp32.c2VjcmV0";
    request.key_id = "approver-esp32";
    request.pubkey = kServerKey;
    request.nonce = kNonce;

    char out[protocol::kRequestMax];
    const size_t n = protocol::BuildRegistrationRequest(request, out, sizeof out);
    TEST_ASSERT_NOT_EQUAL(0, n);
    TEST_ASSERT_EQUAL_UINT32(n, std::strlen(out));

    TEST_ASSERT_NOT_NULL(std::strstr(out, "\"v\":1"));
    TEST_ASSERT_NOT_NULL(std::strstr(out, "\"token\":\"approver-esp32.c2VjcmV0\""));
    TEST_ASSERT_NOT_NULL(std::strstr(out, "\"key_id\":\"approver-esp32\""));
    TEST_ASSERT_NOT_NULL(std::strstr(out, "\"nonce\":\"Tm9uY2VOb25j"));
    TEST_ASSERT_NOT_NULL(std::strstr(out, "\"ts\":1737345600"));
}

// §10.2 pins this device to one scheme, and the allowlist pins it again on the
// verifying side. Nothing this device sends may choose an algorithm, so there is
// no field to vary — the test is that it is there and says the one thing.
//
// **And the one thing is `p256`** (§10.18): what this device registers is an
// ARKG-derived P-256 key, and every verdict is signed inside the security key. A
// device that announced `ed25519` here would have its replies verified against the
// wrong scheme and rejected, one silent approval at a time.
void test_the_request_always_names_p256(void) {
    RegistrationRequest request;
    request.token = "k.s";
    request.key_id = "k";
    request.pubkey = kServerKey;
    request.nonce = kNonce;

    char out[protocol::kRequestMax];
    TEST_ASSERT_NOT_EQUAL(0, protocol::BuildRegistrationRequest(request, out, sizeof out));
    TEST_ASSERT_NOT_NULL(std::strstr(out, "\"key_type\":\"p256\""));
    TEST_ASSERT_NULL(std::strstr(out, "ed25519"));
}

void test_a_request_that_cannot_be_built_writes_nothing(void) {
    RegistrationRequest request;
    request.token = "k.s";
    request.key_id = "k";
    request.pubkey = kServerKey;
    request.nonce = nullptr;

    char out[protocol::kRequestMax];
    std::memset(out, '\x7f', sizeof out);
    TEST_ASSERT_EQUAL_UINT32(0, protocol::BuildRegistrationRequest(request, out, sizeof out));
    TEST_ASSERT_EQUAL_CHAR('\x7f', out[0]);

    // And a buffer too small leaves it alone too.
    request.nonce = kNonce;
    std::memset(out, '\x7f', sizeof out);
    TEST_ASSERT_EQUAL_UINT32(0, protocol::BuildRegistrationRequest(request, out, 16));
    TEST_ASSERT_EQUAL_CHAR('\x7f', out[0]);
}

// ---------------------------------------------------------------------------
// The reply, and the order it is checked in.

void test_a_signed_acceptance_is_read(void) {
    char json[512];
    std::snprintf(json, sizeof json,
                  "{\"v\":1,\"ok\":true,\"key_id\":\"approver-esp32\",\"nonce\":\"%s\","
                  "\"ts\":1737345600,\"server_key\":\"%s\",\"sig\":\"%s\"}",
                  kNonce, kServerKey, kSig);

    Recorder recorder;
    RegistrationReply reply;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ReplyStatus::kVerified),
                          static_cast<int>(Parse(json, &recorder, nullptr, &reply)));

    TEST_ASSERT_TRUE(reply.ok);
    TEST_ASSERT_EQUAL_STRING("approver-esp32", reply.key_id);
    TEST_ASSERT_EQUAL_STRING(kServerKey, reply.server_key);
    TEST_ASSERT_EQUAL_STRING("", reply.error);
    TEST_ASSERT_EQUAL_INT64(1737345600, reply.ts);

    // **The assertion this whole file is built around**: the bytes handed to the
    // verifier are Python's, and the key and signature are the reply's own.
    TEST_ASSERT_EQUAL_INT(1, recorder.calls);
    TEST_ASSERT_EQUAL_STRING(
        "registration-reply\n1\ntrue\napprover-esp32\n"
        "Tm9uY2VOb25jZU5vbmNlTm9uY2VOb25jZU5vbmNlMTI=\n1737345600\n",
        recorder.message);
    TEST_ASSERT_EQUAL_STRING(kServerKey, recorder.public_key);
    TEST_ASSERT_EQUAL_STRING(kSig, recorder.signature);
}

// A signed rejection is a perfectly good reply — `kVerified` is about the
// signature, and `ok` is what says what the handler decided. Reading them as the
// same thing is how a device would treat a forged rejection as gospel.
void test_a_signed_rejection_is_read_and_is_still_a_verified_reply(void) {
    char json[512];
    std::snprintf(json, sizeof json,
                  "{\"v\":1,\"ok\":false,\"key_id\":\"approver-esp32\",\"nonce\":\"%s\","
                  "\"ts\":1737345600,\"error\":\"token unknown\",\"server_key\":\"%s\","
                  "\"sig\":\"%s\"}",
                  kNonce, kServerKey, kSig);

    Recorder recorder;
    RegistrationReply reply;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ReplyStatus::kVerified),
                          static_cast<int>(Parse(json, &recorder, nullptr, &reply)));
    TEST_ASSERT_FALSE(reply.ok);
    TEST_ASSERT_EQUAL_STRING("token unknown", reply.error);
    TEST_ASSERT_EQUAL_STRING(
        "registration-reply\n1\nfalse\napprover-esp32\n"
        "Tm9uY2VOb25jZU5vbmNlTm9uY2VOb25jZU5vbmNlMTI=\n1737345600\ntoken unknown",
        recorder.message);
}

// The signature failing is the one rejection where the verifier *did* run.
void test_a_signature_that_does_not_verify_is_refused(void) {
    char json[512];
    std::snprintf(json, sizeof json,
                  "{\"v\":1,\"ok\":true,\"key_id\":\"k\",\"nonce\":\"%s\",\"ts\":1,"
                  "\"server_key\":\"%s\",\"sig\":\"%s\"}",
                  kNonce, kServerKey, kSig);

    Recorder recorder;
    recorder.answer = false;
    RegistrationReply reply;
    reply.ok = false;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ReplyStatus::kBadSignature),
                          static_cast<int>(Parse(json, &recorder, nullptr, &reply)));
    TEST_ASSERT_EQUAL_INT(1, recorder.calls);
    // Nothing was copied out. A caller that ignored the status would find the
    // struct it passed in untouched rather than an acceptance.
    TEST_ASSERT_FALSE(reply.ok);
    TEST_ASSERT_EQUAL_STRING("", reply.key_id);
}

// --- and everything that is refused *before* the signature is checked --------

void test_a_reply_that_is_not_json_is_refused(void) {
    AssertRejectedBeforeVerifying("not json at all", ReplyStatus::kNotJson);
    AssertRejectedBeforeVerifying("[1,2,3]", ReplyStatus::kNotJson);
    AssertRejectedBeforeVerifying("42", ReplyStatus::kNotJson);
    AssertRejectedBeforeVerifying("\"hello\"", ReplyStatus::kNotJson);
    AssertRejectedBeforeVerifying("", ReplyStatus::kNotJson);
}

void test_a_reply_from_another_protocol_version_is_refused(void) {
    char json[512];
    std::snprintf(json, sizeof json,
                  "{\"v\":2,\"ok\":true,\"nonce\":\"%s\",\"ts\":1,\"server_key\":\"%s\","
                  "\"sig\":\"%s\"}",
                  kNonce, kServerKey, kSig);
    AssertRejectedBeforeVerifying(json, ReplyStatus::kBadVersion);
}

void test_a_reply_that_says_neither_yes_nor_no_is_refused(void) {
    char json[512];
    // `"ok":"true"` — a string, which is not a decision. The check is
    // `cJSON_IsBool` rather than truthiness for exactly this.
    std::snprintf(json, sizeof json,
                  "{\"v\":1,\"ok\":\"true\",\"nonce\":\"%s\",\"ts\":1,\"server_key\":\"%s\","
                  "\"sig\":\"%s\"}",
                  kNonce, kServerKey, kSig);
    AssertRejectedBeforeVerifying(json, ReplyStatus::kNoOk);

    std::snprintf(json, sizeof json,
                  "{\"v\":1,\"nonce\":\"%s\",\"ts\":1,\"server_key\":\"%s\",\"sig\":\"%s\"}", kNonce,
                  kServerKey, kSig);
    AssertRejectedBeforeVerifying(json, ReplyStatus::kNoOk);
}

void test_a_reply_with_no_handler_key_is_refused(void) {
    char json[512];
    std::snprintf(json, sizeof json, "{\"v\":1,\"ok\":true,\"nonce\":\"%s\",\"ts\":1,\"sig\":\"%s\"}",
                  kNonce, kSig);
    AssertRejectedBeforeVerifying(json, ReplyStatus::kNoServerKey);

    std::snprintf(json, sizeof json,
                  "{\"v\":1,\"ok\":true,\"nonce\":\"%s\",\"ts\":1,\"server_key\":\"\","
                  "\"sig\":\"%s\"}",
                  kNonce, kSig);
    AssertRejectedBeforeVerifying(json, ReplyStatus::kNoServerKey);
}

// **The pin.** A valid signature by a key this device does not already trust is
// not a bad signature — it is somebody else taking over the slot, and it is
// refused before the signature is even looked at so that the console can say
// which of the two happened.
void test_a_reply_signed_by_a_different_key_than_the_pinned_one_is_refused(void) {
    char json[512];
    std::snprintf(json, sizeof json,
                  "{\"v\":1,\"ok\":true,\"key_id\":\"k\",\"nonce\":\"%s\",\"ts\":1,"
                  "\"server_key\":\"%s\",\"sig\":\"%s\"}",
                  kNonce, kOtherKey, kSig);

    bool called = false;
    RegistrationReply out;
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(ReplyStatus::kWrongServerKey),
        static_cast<int>(protocol::ParseRegistrationReply(json, std::strlen(json), kNonce,
                                                          kServerKey, &NeverCalled, &called, &out)));
    TEST_ASSERT_FALSE(called);
}

// And the same reply with the *matching* pin goes through, so the test above is
// about the pin rather than about that key being unusable.
void test_a_reply_signed_by_the_pinned_key_is_taken(void) {
    char json[512];
    std::snprintf(json, sizeof json,
                  "{\"v\":1,\"ok\":true,\"key_id\":\"k\",\"nonce\":\"%s\",\"ts\":1,"
                  "\"server_key\":\"%s\",\"sig\":\"%s\"}",
                  kNonce, kServerKey, kSig);
    Recorder recorder;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ReplyStatus::kVerified),
                          static_cast<int>(Parse(json, &recorder, kServerKey)));
}

// An empty pin is a first registration — trust on first use, closed by the
// operator comparing the string with what the handler printed (§10.7).
void test_no_pin_takes_the_key_on_trust(void) {
    char json[512];
    std::snprintf(json, sizeof json,
                  "{\"v\":1,\"ok\":true,\"key_id\":\"k\",\"nonce\":\"%s\",\"ts\":1,"
                  "\"server_key\":\"%s\",\"sig\":\"%s\"}",
                  kNonce, kOtherKey, kSig);
    Recorder recorder;
    RegistrationReply reply;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ReplyStatus::kVerified),
                          static_cast<int>(Parse(json, &recorder, "", &reply)));
    TEST_ASSERT_EQUAL_STRING(kOtherKey, reply.server_key);
}

// The nonce echo is what makes a reply usable exactly once, for this exchange.
void test_a_reply_to_a_different_request_is_refused(void) {
    char json[512];
    std::snprintf(json, sizeof json,
                  "{\"v\":1,\"ok\":true,\"nonce\":\"c29tZXRoaW5nIGVsc2U=\",\"ts\":1,"
                  "\"server_key\":\"%s\",\"sig\":\"%s\"}",
                  kServerKey, kSig);
    AssertRejectedBeforeVerifying(json, ReplyStatus::kNonceMismatch);

    std::snprintf(json, sizeof json,
                  "{\"v\":1,\"ok\":true,\"ts\":1,\"server_key\":\"%s\",\"sig\":\"%s\"}", kServerKey,
                  kSig);
    AssertRejectedBeforeVerifying(json, ReplyStatus::kNonceMismatch);
}

void test_a_reply_with_no_timestamp_is_refused(void) {
    char json[512];
    std::snprintf(json, sizeof json,
                  "{\"v\":1,\"ok\":true,\"nonce\":\"%s\",\"server_key\":\"%s\",\"sig\":\"%s\"}",
                  kNonce, kServerKey, kSig);
    AssertRejectedBeforeVerifying(json, ReplyStatus::kNoTimestamp);
}

// **A JSON number is a double.** A `ts` past 2^53 is silently rounded by any
// parser, which reproduces different signing bytes and a signature that just
// stops verifying, with nothing to point at. Refusing it names the problem.
void test_a_timestamp_too_large_to_be_exact_is_refused(void) {
    char json[512];
    std::snprintf(json, sizeof json,
                  "{\"v\":1,\"ok\":true,\"nonce\":\"%s\",\"ts\":1e300,\"server_key\":\"%s\","
                  "\"sig\":\"%s\"}",
                  kNonce, kServerKey, kSig);
    AssertRejectedBeforeVerifying(json, ReplyStatus::kNoTimestamp);

    // And one that is not an integer at all.
    std::snprintf(json, sizeof json,
                  "{\"v\":1,\"ok\":true,\"nonce\":\"%s\",\"ts\":1.5,\"server_key\":\"%s\","
                  "\"sig\":\"%s\"}",
                  kNonce, kServerKey, kSig);
    AssertRejectedBeforeVerifying(json, ReplyStatus::kNoTimestamp);

    // **2^53 + 1, and this is the case the range check exists for** — it is what
    // the round trip below it cannot see. The parser lands on 2^53, which then
    // survives a truncate-and-compare perfectly happily, so without the explicit
    // bound the device would sign `…992` against a handler that signed `…993` and
    // the signature would fail with nothing to point at.
    //
    // **And 2^53 itself has to go with it**, which is what this test got wrong
    // the first time: the two parse to the *same* double, so from here they are
    // indistinguishable and no rule can take one and refuse the other. The bound
    // is therefore exclusive — below it every integer is its own value.
    std::snprintf(json, sizeof json,
                  "{\"v\":1,\"ok\":true,\"nonce\":\"%s\",\"ts\":9007199254740993,"
                  "\"server_key\":\"%s\",\"sig\":\"%s\"}",
                  kNonce, kServerKey, kSig);
    AssertRejectedBeforeVerifying(json, ReplyStatus::kNoTimestamp);

    std::snprintf(json, sizeof json,
                  "{\"v\":1,\"ok\":true,\"nonce\":\"%s\",\"ts\":9007199254740992,"
                  "\"server_key\":\"%s\",\"sig\":\"%s\"}",
                  kNonce, kServerKey, kSig);
    AssertRejectedBeforeVerifying(json, ReplyStatus::kNoTimestamp);

    // One below it goes through, so this is a boundary rather than a refusal of
    // anything large — and a timestamp in seconds reaches it in the year 285
    // million, which is a long time to be worrying about a clock.
    std::snprintf(json, sizeof json,
                  "{\"v\":1,\"ok\":true,\"nonce\":\"%s\",\"ts\":9007199254740991,"
                  "\"server_key\":\"%s\",\"sig\":\"%s\"}",
                  kNonce, kServerKey, kSig);
    Recorder recorder;
    RegistrationReply reply;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ReplyStatus::kVerified),
                          static_cast<int>(Parse(json, &recorder, nullptr, &reply)));
    TEST_ASSERT_EQUAL_INT64(9007199254740991LL, reply.ts);
}

// Everything off an open subject is attacker-shaped (§10.10): a field longer than
// this device will hold is dropped rather than truncated onto a console.
void test_a_field_longer_than_this_device_will_hold_is_refused(void) {
    char error[protocol::kErrorMax + 40];
    std::memset(error, 'e', sizeof error);
    error[sizeof error - 1] = '\0';

    char json[1024];
    std::snprintf(json, sizeof json,
                  "{\"v\":1,\"ok\":false,\"key_id\":\"k\",\"error\":\"%s\",\"nonce\":\"%s\","
                  "\"ts\":1,\"server_key\":\"%s\",\"sig\":\"%s\"}",
                  error, kNonce, kServerKey, kSig);
    AssertRejectedBeforeVerifying(json, ReplyStatus::kBadFields);

    // A `server_key` too long is caught one step earlier, where the key is read.
    char key[protocol::kB64_32Max + 20];
    std::memset(key, 'A', sizeof key);
    key[sizeof key - 1] = '\0';
    std::snprintf(json, sizeof json,
                  "{\"v\":1,\"ok\":true,\"nonce\":\"%s\",\"ts\":1,\"server_key\":\"%s\","
                  "\"sig\":\"%s\"}",
                  kNonce, key, kSig);
    AssertRejectedBeforeVerifying(json, ReplyStatus::kNoServerKey);
}

void test_a_reply_with_no_signature_is_refused(void) {
    char json[512];
    std::snprintf(json, sizeof json,
                  "{\"v\":1,\"ok\":true,\"nonce\":\"%s\",\"ts\":1,\"server_key\":\"%s\"}", kNonce,
                  kServerKey);
    AssertRejectedBeforeVerifying(json, ReplyStatus::kBadSignature);

    // And one whose signature could not be one — 88 characters is the whole of a
    // base64 Ed25519 signature, so anything longer is not a truncated read to
    // attempt, it is a field to drop.
    char sig[protocol::kB64_64Max + 20];
    std::memset(sig, 'A', sizeof sig);
    sig[sizeof sig - 1] = '\0';
    std::snprintf(json, sizeof json,
                  "{\"v\":1,\"ok\":true,\"nonce\":\"%s\",\"ts\":1,\"server_key\":\"%s\","
                  "\"sig\":\"%s\"}",
                  kNonce, kServerKey, sig);
    AssertRejectedBeforeVerifying(json, ReplyStatus::kBadSignature);
}

// The handler omits `error` on success and can omit `key_id` on a rejection, and
// both are signed as `""` on the other side. Absent and empty have to be the same
// thing here or the signature does not reproduce.
void test_an_absent_optional_field_signs_as_empty(void) {
    char json[512];
    std::snprintf(json, sizeof json,
                  "{\"v\":1,\"ok\":false,\"nonce\":\"%s\",\"ts\":-1,\"server_key\":\"%s\","
                  "\"sig\":\"%s\"}",
                  kNonce, kServerKey, kSig);

    Recorder recorder;
    RegistrationReply reply;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ReplyStatus::kVerified),
                          static_cast<int>(Parse(json, &recorder, nullptr, &reply)));
    TEST_ASSERT_EQUAL_STRING("", reply.key_id);
    TEST_ASSERT_EQUAL_STRING("", reply.error);
    TEST_ASSERT_EQUAL_STRING(
        "registration-reply\n1\nfalse\n\nTm9uY2VOb25jZU5vbmNlTm9uY2VOb25jZU5vbmNlMTI=\n-1\n",
        recorder.message);
}

// Every status needs its own sentence, because "the handler said no" and
// "somebody else on the network said no" read identically otherwise.
void test_every_status_has_something_to_say(void) {
    const ReplyStatus all[] = {
        ReplyStatus::kVerified,      ReplyStatus::kNotJson,      ReplyStatus::kBadVersion,
        ReplyStatus::kNoOk,          ReplyStatus::kNoServerKey,  ReplyStatus::kWrongServerKey,
        ReplyStatus::kNonceMismatch, ReplyStatus::kNoTimestamp,  ReplyStatus::kBadFields,
        ReplyStatus::kTooLong,       ReplyStatus::kBadSignature,
    };
    for (const ReplyStatus status : all) {
        const char *text = protocol::ReplyStatusText(status);
        TEST_ASSERT_NOT_NULL(text);
        TEST_ASSERT_TRUE(std::strlen(text) > 8);
        TEST_ASSERT_NULL_MESSAGE(std::strchr(text, '\xa7'), "a section sign reached the operator");
    }
}

}  // namespace

void RegisterRegistrationTests(void) {
    RUN_TEST(test_the_ok_flag_is_spelled_the_way_json_spells_it);
    RUN_TEST(test_reply_signing_bytes_refuse_what_does_not_fit);

    RUN_TEST(test_a_token_splits_at_the_first_dot);
    RUN_TEST(test_a_token_that_is_not_one_is_refused);

    RUN_TEST(test_the_request_carries_what_six_asks_for);
    RUN_TEST(test_the_request_always_names_p256);
    RUN_TEST(test_a_request_that_cannot_be_built_writes_nothing);

    RUN_TEST(test_a_signed_acceptance_is_read);
    RUN_TEST(test_a_signed_rejection_is_read_and_is_still_a_verified_reply);
    RUN_TEST(test_a_signature_that_does_not_verify_is_refused);

    RUN_TEST(test_a_reply_that_is_not_json_is_refused);
    RUN_TEST(test_a_reply_from_another_protocol_version_is_refused);
    RUN_TEST(test_a_reply_that_says_neither_yes_nor_no_is_refused);
    RUN_TEST(test_a_reply_with_no_handler_key_is_refused);
    RUN_TEST(test_a_reply_signed_by_a_different_key_than_the_pinned_one_is_refused);
    RUN_TEST(test_a_reply_signed_by_the_pinned_key_is_taken);
    RUN_TEST(test_no_pin_takes_the_key_on_trust);
    RUN_TEST(test_a_reply_to_a_different_request_is_refused);
    RUN_TEST(test_a_reply_with_no_timestamp_is_refused);
    RUN_TEST(test_a_timestamp_too_large_to_be_exact_is_refused);
    RUN_TEST(test_a_field_longer_than_this_device_will_hold_is_refused);
    RUN_TEST(test_a_reply_with_no_signature_is_refused);
    RUN_TEST(test_an_absent_optional_field_signs_as_empty);
    RUN_TEST(test_every_status_has_something_to_say);
}
