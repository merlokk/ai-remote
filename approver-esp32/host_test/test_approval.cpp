// §7 on the wire (CLAUDE.md §10.11, host tier; §10.2, §10.8.4) — what arrives on
// `approvals.*` and what goes back.
//
// The request below is **`hook.py`'s own output**, not something written to match
// the parser: `build_request` was run on this machine with a realistic payload and
// the JSON pasted in. A fixture invented here would pass for a parser that agrees
// with this file and with nothing else.
//
// What is pinned, and the two halves are different kinds of claim:
//
//   * **the parse** — every field lands where §7 says, `tool_input` is rendered
//     whole rather than reached into, and every refusal is its own case with
//     nothing written into the caller's card. The refusals are the half that
//     matters: this is the one subject on the device that anybody on the LAN can
//     publish to (§10.3), so junk is ordinary traffic and each kind of junk must
//     cost one log line and no reply (§10.10);
//   * **the reply** — the six fields `hook.py::verify_reply` compares against
//     what it sent are echoed exactly, `reason` is empty, `updated_input` is
//     absent, and a behaviour §7 has no word for never reaches the wire.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>

#include "approval.h"
#include "signing.h"   // kBehaviorAllow / kBehaviorDeny
#include "unity.h"

using protocol::RequestStatus;
using ui::Request;

namespace {

// --- `hook.py::build_request`, verbatim -------------------------------------

constexpr char kRequest[] =
    "{\"v\": 1, \"session_id\": \"4f9a2c1e-77b3-4d0a-9f21-8c6e5b3a1d02\", "
    "\"tool_name\": \"Bash\", \"tool_input\": {\"command\": \"rm -rf /tmp/build\", "
    "\"description\": \"clean\"}, "
    "\"input_sha256\": \"e8b70996597e349ff8cb37b6cbe3ae52f6c64e27bfb3f41edf819c4817501f14\", "
    "\"permission_mode\": \"default\", \"cwd\": \"E:\\\\projects\\\\ai-remote\", "
    "\"nonce\": \"Tm9uY2VOb25jZU5vbmNlTm9uY2VOb25jZU5vbmNlMTI=\", \"ts\": 1737345600}";

constexpr char kInbox[] = "_INBOX.b0a56e1a6a770cf68cebfadcf57c4831";

constexpr char kPoison = '\x7f';

RequestStatus Parse(const char *json, Request *out, const char *reply = kInbox) {
    return protocol::ParseApprovalRequest(json, std::strlen(json), reply, out);
}

// Every refusal must leave the caller's card exactly as it found it — the card on
// the glass belongs to a request somebody is still looking at, and a bad message
// must not half-overwrite it.
void AssertRefused(const char *json, RequestStatus expected, const char *reply = kInbox) {
    Request card;
    std::memset(card.tool_name, kPoison, sizeof card.tool_name);
    std::memset(card.session_id, kPoison, sizeof card.session_id);
    card.ts = 4242;

    TEST_ASSERT_EQUAL_INT(static_cast<int>(expected),
                          static_cast<int>(protocol::ParseApprovalRequest(
                              json, std::strlen(json), reply, &card)));
    TEST_ASSERT_EQUAL_CHAR(kPoison, card.tool_name[0]);
    TEST_ASSERT_EQUAL_CHAR(kPoison, card.session_id[0]);
    TEST_ASSERT_EQUAL_INT64(4242, card.ts);
}

// Builds a request JSON with one field replaced or removed, so each case below is
// one difference from something known to work.
void Fill(char *out, size_t capacity, const char *tool_input, const char *ts, const char *extra) {
    std::snprintf(out, capacity,
                  "{\"v\":1,\"session_id\":\"s\",\"tool_name\":\"Bash\",\"tool_input\":%s,"
                  "\"input_sha256\":\"%s\",\"cwd\":\"/tmp\",\"nonce\":\"n\",\"ts\":%s%s}",
                  tool_input,
                  "e8b70996597e349ff8cb37b6cbe3ae52f6c64e27bfb3f41edf819c4817501f14", ts, extra);
}

// ---------------------------------------------------------------------------
// The parse.

void test_a_real_hook_request_becomes_a_card(void) {
    Request card;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(RequestStatus::kOk),
                          static_cast<int>(Parse(kRequest, &card)));

    TEST_ASSERT_EQUAL_INT(1, card.v);
    TEST_ASSERT_EQUAL_INT64(1737345600, card.ts);
    TEST_ASSERT_EQUAL_STRING("4f9a2c1e-77b3-4d0a-9f21-8c6e5b3a1d02", card.session_id);
    TEST_ASSERT_EQUAL_STRING("Bash", card.tool_name);
    TEST_ASSERT_EQUAL_STRING("e8b70996597e349ff8cb37b6cbe3ae52f6c64e27bfb3f41edf819c4817501f14",
                             card.input_sha256);
    TEST_ASSERT_EQUAL_STRING("Tm9uY2VOb25jZU5vbmNlTm9uY2VOb25jZU5vbmNlMTI=", card.nonce);
    TEST_ASSERT_EQUAL_STRING("E:\\projects\\ai-remote", card.cwd);
    TEST_ASSERT_EQUAL_STRING(kInbox, card.reply);
}

// §10.8.4 forbids showing part of what is being asked for, so the whole
// `tool_input` object is rendered rather than reached into for a `command`. What
// the operator reads is the structure `input_sha256` was computed over.
void test_the_whole_tool_input_is_shown(void) {
    Request card;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(RequestStatus::kOk),
                          static_cast<int>(Parse(kRequest, &card)));
    TEST_ASSERT_EQUAL_STRING("{\"command\":\"rm -rf /tmp/build\",\"description\":\"clean\"}",
                             card.tool_input);
}

// The hook does not put its timeout on the wire, so the card's own default is
// what expires one — and zero here is what selects it (`kDefaultTtlMs`).
void test_no_ttl_arrives_so_the_card_uses_its_own(void) {
    Request card;
    card.ttl_ms = 12345;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(RequestStatus::kOk),
                          static_cast<int>(Parse(kRequest, &card)));
    TEST_ASSERT_EQUAL_UINT32(0, card.ttl_ms);
}

// `permission_mode` is on the wire and is neither shown nor signed. Reading it
// would be a field this device has an opinion about, and it has none.
void test_a_field_this_device_does_not_use_is_ignored(void) {
    Request card;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(RequestStatus::kOk),
                          static_cast<int>(Parse(kRequest, &card)));
    // Nothing to assert but the parse succeeding — which is the point: an unknown
    // or unused field must not be a refusal, or a newer hook breaks this device.
}

// --- and every way it is not a card ----------------------------------------

// §10.10: a request with nowhere to answer must not become a card, because a
// press on it could never reach anybody.
void test_a_request_with_nowhere_to_answer_is_refused(void) {
    AssertRefused(kRequest, RequestStatus::kNoReply, "");
    AssertRefused(kRequest, RequestStatus::kNoReply, nullptr);
}

void test_a_reply_subject_too_long_to_hold_is_refused(void) {
    char inbox[ui::kReplySubjectSize + 16];
    std::memset(inbox, 'x', sizeof inbox);
    inbox[sizeof inbox - 1] = '\0';
    AssertRefused(kRequest, RequestStatus::kTooLong, inbox);
}

void test_a_payload_that_is_not_an_object_is_refused(void) {
    AssertRefused("not json", RequestStatus::kNotJson);
    AssertRefused("", RequestStatus::kNotJson);
    AssertRefused("[1,2,3]", RequestStatus::kNotJson);
    AssertRefused("42", RequestStatus::kNotJson);
    AssertRefused("\"hello\"", RequestStatus::kNotJson);
}

void test_another_protocol_version_is_refused(void) {
    char json[512];
    Fill(json, sizeof json, "{}", "1", ",\"v_unused\":0");
    // Rewrite the version rather than the whole fixture.
    char *v = std::strstr(json, "\"v\":1");
    v[4] = '2';
    AssertRefused(json, RequestStatus::kBadVersion);
}

// Each required field, one at a time, so a field added later cannot skip the
// check by being absent from a hand-written list.
void test_a_missing_field_is_refused(void) {
    const char *fields[] = {"session_id", "tool_name", "input_sha256", "nonce", "tool_input"};
    for (const char *field : fields) {
        char json[512];
        Fill(json, sizeof json, "{}", "1", "");

        // Rename the field so it is no longer found, keeping the JSON valid.
        char *at = std::strstr(json, field);
        TEST_ASSERT_NOT_NULL(at);
        at[0] = 'X';

        Request card;
        const RequestStatus status = Parse(json, &card);
        TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(RequestStatus::kMissingField),
                                      static_cast<int>(status), field);
    }
}

void test_a_field_longer_than_the_card_will_hold_is_refused(void) {
    char tool[ui::kToolNameSize + 8];
    std::memset(tool, 't', sizeof tool);
    tool[sizeof tool - 1] = '\0';

    char json[2048];
    std::snprintf(json, sizeof json,
                  "{\"v\":1,\"session_id\":\"s\",\"tool_name\":\"%s\",\"tool_input\":{},"
                  "\"input_sha256\":\"a\",\"nonce\":\"n\",\"ts\":1}",
                  tool);
    AssertRefused(json, RequestStatus::kTooLong);
}

// **The one refusal with a stated cost** (§10.8.4): a `Write` of a file bigger
// than the card's buffer produces no card and no reply, and the question goes
// back to Claude Code's own terminal. Refusing is the rule; truncating a command
// into something that reads as harmless is what it exists to prevent.
void test_a_tool_input_too_big_to_show_whole_is_refused(void) {
    static char json[ui::kToolInputSize * 2];
    static char big[ui::kToolInputSize + 64];
    std::memset(big, 'x', sizeof big);
    big[sizeof big - 1] = '\0';

    std::snprintf(json, sizeof json,
                  "{\"v\":1,\"session_id\":\"s\",\"tool_name\":\"Write\","
                  "\"tool_input\":{\"content\":\"%s\"},"
                  "\"input_sha256\":\"a\",\"nonce\":\"n\",\"ts\":1}",
                  big);
    AssertRefused(json, RequestStatus::kTooLong);
}

// A `ts` at or past 2^53 cannot be echoed exactly, and echoing it wrong means a
// signature over different bytes than the hook signed — a failure with nothing to
// point at. The same rule the registration reply follows, for the same reason.
void test_a_timestamp_that_cannot_be_echoed_exactly_is_refused(void) {
    char json[512];
    Fill(json, sizeof json, "{}", "9007199254740992", "");
    AssertRefused(json, RequestStatus::kBadTimestamp);

    Fill(json, sizeof json, "{}", "1.5", "");
    AssertRefused(json, RequestStatus::kBadTimestamp);

    Fill(json, sizeof json, "{}", "\"soon\"", "");
    AssertRefused(json, RequestStatus::kBadTimestamp);
}

// The hook sends `cwd: null` when it has none, and a card without a working
// directory is still a card worth answering.
void test_an_absent_cwd_is_not_a_refusal(void) {
    char json[512];
    std::snprintf(json, sizeof json,
                  "{\"v\":1,\"session_id\":\"s\",\"tool_name\":\"Bash\",\"tool_input\":{},"
                  "\"input_sha256\":\"a\",\"cwd\":null,\"nonce\":\"n\",\"ts\":1}");
    Request card;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(RequestStatus::kOk),
                          static_cast<int>(Parse(json, &card)));
    TEST_ASSERT_EQUAL_STRING("", card.cwd);
}

void test_every_status_has_something_to_say(void) {
    const RequestStatus all[] = {
        RequestStatus::kOk,       RequestStatus::kNotJson,  RequestStatus::kBadVersion,
        RequestStatus::kMissingField, RequestStatus::kTooLong,  RequestStatus::kNoReply,
        RequestStatus::kBadTimestamp,
    };
    for (const RequestStatus status : all) {
        const char *text = protocol::RequestStatusText(status);
        TEST_ASSERT_NOT_NULL(text);
        TEST_ASSERT_TRUE(std::strlen(text) >= 2);
    }
}

// ---------------------------------------------------------------------------
// The reply.

Request Sample() {
    Request card;
    Parse(kRequest, &card);
    return card;
}

// `hook.py::verify_reply` compares `v`, `session_id`, `tool_name`, `input_sha256`,
// `nonce` and `ts` against what it sent, one by one, before it looks at the
// signature. Any one of them wrong is a reply the hook drops.
void test_the_reply_echoes_every_field_the_hook_checks(void) {
    const Request card = Sample();
    char out[protocol::kDecisionReplyMax];
    const size_t n = protocol::BuildDecisionReply(card, protocol::kBehaviorAllow, "approver-esp32",
                                                  "c2lnbmF0dXJl", out, sizeof out);
    TEST_ASSERT_NOT_EQUAL(0, n);
    TEST_ASSERT_EQUAL_UINT32(n, std::strlen(out));

    TEST_ASSERT_NOT_NULL(std::strstr(out, "\"v\":1"));
    TEST_ASSERT_NOT_NULL(std::strstr(out, "\"session_id\":\"4f9a2c1e-77b3-4d0a-9f21-8c6e5b3a1d02\""));
    TEST_ASSERT_NOT_NULL(std::strstr(out, "\"tool_name\":\"Bash\""));
    TEST_ASSERT_NOT_NULL(std::strstr(
        out, "\"input_sha256\":\"e8b70996597e349ff8cb37b6cbe3ae52f6c64e27bfb3f41edf819c4817501f14\""));
    TEST_ASSERT_NOT_NULL(std::strstr(out, "\"nonce\":\"Tm9uY2VOb25jZU5vbmNlTm9uY2VOb25jZU5vbmNlMTI=\""));
    TEST_ASSERT_NOT_NULL(std::strstr(out, "\"ts\":1737345600"));

    TEST_ASSERT_NOT_NULL(std::strstr(out, "\"behavior\":\"allow\""));
    TEST_ASSERT_NOT_NULL(std::strstr(out, "\"key_id\":\"approver-esp32\""));
    TEST_ASSERT_NOT_NULL(std::strstr(out, "\"sig\":\"c2lnbmF0dXJl\""));
}

// §10.2: `reason` is always empty and `updated_input` is never sent. The second is
// the load-bearing one — its absence is what keeps `updated_input_sha256` empty in
// the signed bytes, and the hook recomputes that from the reply.
void test_the_reply_carries_an_empty_reason_and_no_updated_input(void) {
    const Request card = Sample();
    char out[protocol::kDecisionReplyMax];
    protocol::BuildDecisionReply(card, protocol::kBehaviorDeny, "approver-esp32", "c2ln", out,
                                 sizeof out);
    TEST_ASSERT_NOT_NULL(std::strstr(out, "\"reason\":\"\""));
    TEST_ASSERT_NULL(std::strstr(out, "updated_input"));
    TEST_ASSERT_NOT_NULL(std::strstr(out, "\"behavior\":\"deny\""));
}

void test_a_behaviour_this_protocol_does_not_have_never_reaches_the_wire(void) {
    const Request card = Sample();
    char out[protocol::kDecisionReplyMax];
    for (const char *behavior : {"", "ALLOW", "allowed", "skip"}) {
        std::memset(out, kPoison, sizeof out);
        TEST_ASSERT_EQUAL_UINT32(
            0, protocol::BuildDecisionReply(card, behavior, "approver-esp32", "c2ln", out,
                                            sizeof out));
        TEST_ASSERT_EQUAL_CHAR(kPoison, out[0]);
    }
}

void test_a_reply_that_does_not_fit_writes_nothing(void) {
    const Request card = Sample();
    char out[protocol::kDecisionReplyMax];
    std::memset(out, kPoison, sizeof out);
    TEST_ASSERT_EQUAL_UINT32(0, protocol::BuildDecisionReply(card, protocol::kBehaviorAllow,
                                                             "approver-esp32", "c2ln", out, 32));
    TEST_ASSERT_EQUAL_CHAR(kPoison, out[0]);
}

// The bound in the header has to hold a reply built from a card at its limits, or
// the refusal above becomes reachable with an ordinary request.
void test_the_declared_bound_holds_a_card_at_its_limits(void) {
    Request card;
    card.v = 1;
    card.ts = -9007199254740991LL;
    std::memset(card.session_id, 's', sizeof card.session_id - 1);
    std::memset(card.nonce, 'n', sizeof card.nonce - 1);
    std::memset(card.tool_name, 't', sizeof card.tool_name - 1);
    std::memset(card.input_sha256, 'a', sizeof card.input_sha256 - 1);

    // 88 characters plus a terminator, which is base64 of an Ed25519 signature.
    // Spelled out rather than taken from `crypto::kSignatureB64Size`: that header
    // is libsodium's and this suite does not link it.
    char signature[89];
    std::memset(signature, 'A', sizeof signature - 1);
    signature[sizeof signature - 1] = '\0';

    char out[protocol::kDecisionReplyMax];
    TEST_ASSERT_NOT_EQUAL(0, protocol::BuildDecisionReply(card, protocol::kBehaviorAllow,
                                                          "approver-esp32", signature, out,
                                                          sizeof out));
}

}  // namespace

void RegisterApprovalTests(void) {
    RUN_TEST(test_a_real_hook_request_becomes_a_card);
    RUN_TEST(test_the_whole_tool_input_is_shown);
    RUN_TEST(test_no_ttl_arrives_so_the_card_uses_its_own);
    RUN_TEST(test_a_field_this_device_does_not_use_is_ignored);

    RUN_TEST(test_a_request_with_nowhere_to_answer_is_refused);
    RUN_TEST(test_a_reply_subject_too_long_to_hold_is_refused);
    RUN_TEST(test_a_payload_that_is_not_an_object_is_refused);
    RUN_TEST(test_another_protocol_version_is_refused);
    RUN_TEST(test_a_missing_field_is_refused);
    RUN_TEST(test_a_field_longer_than_the_card_will_hold_is_refused);
    RUN_TEST(test_a_tool_input_too_big_to_show_whole_is_refused);
    RUN_TEST(test_a_timestamp_that_cannot_be_echoed_exactly_is_refused);
    RUN_TEST(test_an_absent_cwd_is_not_a_refusal);
    RUN_TEST(test_every_status_has_something_to_say);

    RUN_TEST(test_the_reply_echoes_every_field_the_hook_checks);
    RUN_TEST(test_the_reply_carries_an_empty_reason_and_no_updated_input);
    RUN_TEST(test_a_behaviour_this_protocol_does_not_have_never_reaches_the_wire);
    RUN_TEST(test_a_reply_that_does_not_fit_writes_nothing);
    RUN_TEST(test_the_declared_bound_holds_a_card_at_its_limits);
}
