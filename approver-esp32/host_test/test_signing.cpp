// The bytes a decision is signed over (CLAUDE.md §10.11, host tier; §10.2, §7).
//
// **This is the suite with the least room to be approximately right.** Every
// other host test protects a behaviour somebody would notice going wrong; this
// one protects an exact byte string, and the failure it guards against is
// invisible from the device's own side — a signature the hook rejects looks like
// a responder that is simply not answering, and Claude Code keeps asking in its
// own terminal.
//
// **The complete messages moved out of this file, and that is tier 2 arriving.**
// They used to be three string literals here — Python's own output, pasted in,
// which is better than deriving the expectation from `signing.h` and worse than
// it looks: a pasted literal cannot be checked for staleness, so it would go on
// passing forever after `protocol.py` changed underneath it. They now live in
// `vectors/parity_vectors.h`, which is **generated** from the Python side and
// checked against it on every `pytest` run, and `test_vectors.cpp` is what runs
// the assembler over them.
//
// What stays here is everything that is a claim about *this* implementation
// rather than about the other language:
//
//   * the two empty fields keeping their **positions**: `updated_input_sha256`
//     between two separators and `reason` at the tail, which is what makes the
//     message end in `\n` and is the single easiest thing to lose while tidying;
//   * exactly eight separators, always, whatever the fields hold;
//   * every refusal writing **nothing** — the rule `endpoint.h` states about a
//     bad URL, which matters more here because a half-assembled buffer is
//     something a caller could sign;
//   * `AppendInt` on its own, `INT64_MIN` included: the value with no positive
//     counterpart, and the one the obvious negate-and-divide loop gets wrong.
//
// The sample request the refusals are built from is still the parity vector, so
// there is one description of a realistic decision in this directory rather than
// two that can drift.

#include <cstdint>
#include <cstring>

#include "signing.h"
#include "unity.h"
#include "vectors/parity_vectors.h"

using protocol::Decision;
using protocol::kBehaviorAllow;
using protocol::kBehaviorDeny;
using protocol::kSigningBytesMax;

namespace {

// ---------------------------------------------------------------------------
// The sample, from the generated parity vectors.

// The realistic `Bash` approval, as `approver/protocol.py` describes it. Reached
// through the lookup rather than by index so a reordered generator cannot quietly
// point this whole file at a different request.
const vectors::DecisionVector &Vector() {
    const vectors::DecisionVector *found = vectors::FindDecision("allow-bash");
    TEST_ASSERT_NOT_NULL_MESSAGE(found, "the allow-bash parity vector is gone");
    return *found;
}

Decision Sample() {
    const vectors::DecisionVector &v = Vector();
    Decision d;
    d.v = v.v;
    d.ts = v.ts;
    d.session_id = v.session_id;
    d.nonce = v.nonce;
    d.tool_name = v.tool_name;
    d.input_sha256 = v.input_sha256;
    d.behavior = v.behavior;
    return d;
}

// A buffer poisoned before every call, so "wrote nothing" is a fact rather than
// an assumption about what happened to be there.
constexpr char kPoison = '\x7f';

size_t Assemble(const Decision &d, char *out, size_t size = kSigningBytesMax) {
    std::memset(out, kPoison, kSigningBytesMax);
    return protocol::DecisionSigningBytes(d, out, size);
}

void AssertRefused(const Decision &d) {
    char out[kSigningBytesMax];
    TEST_ASSERT_EQUAL_UINT32(0, Assemble(d, out));
    for (size_t i = 0; i < sizeof out; ++i) {
        TEST_ASSERT_EQUAL_CHAR(kPoison, out[i]);
    }
}

size_t CountSeparators(const char *text, size_t length) {
    size_t n = 0;
    for (size_t i = 0; i < length; ++i) {
        if (text[i] == '\n') {
            ++n;
        }
    }
    return n;
}

// ---------------------------------------------------------------------------
// The shape of the message, independently of what is in it.
//
// The complete messages are `test_vectors.cpp`'s job now. What is below is what
// stays true whatever the fields hold, which is a different kind of claim and the
// one a generated fixture cannot make.

// §10.2: `updated_input_sha256` is a position, not a value. If somebody ever
// "tidies" the empty write away, this is what fails — and it fails before any
// signature is ever rejected in the field.
void test_the_two_empty_fields_keep_their_positions(void) {
    char out[kSigningBytesMax];
    const size_t n = Assemble(Sample(), out);

    // Two separators in a row, which is the empty `updated_input_sha256` between
    // `behavior` and `ts`.
    TEST_ASSERT_NOT_NULL(std::strstr(out, "\nallow\n\n"));

    // And `reason`, last and empty, is why the whole thing ends in a separator
    // with nothing after it.
    TEST_ASSERT_EQUAL_CHAR('\n', out[n - 1]);
}

void test_there_are_always_exactly_eight_separators(void) {
    char out[kSigningBytesMax];

    size_t n = Assemble(Sample(), out);
    TEST_ASSERT_EQUAL_UINT32(8, CountSeparators(out, n));

    Decision d = Sample();
    d.session_id = "s";
    d.nonce = "n";
    d.tool_name = "X";
    d.input_sha256 = "";
    d.behavior = kBehaviorDeny;
    d.ts = 0;
    n = Assemble(d, out);
    TEST_ASSERT_EQUAL_UINT32(8, CountSeparators(out, n));
}

// The length is not a terminator's business: the signature is over bytes.
void test_the_length_excludes_the_terminator_and_the_buffer_still_has_one(void) {
    char out[kSigningBytesMax];
    const size_t n = Assemble(Sample(), out);
    TEST_ASSERT_EQUAL_CHAR('\0', out[n]);
    TEST_ASSERT_EQUAL_UINT32(n, std::strlen(out));
}

// ---------------------------------------------------------------------------
// Every refusal, and each of them writing nothing.

void test_a_missing_field_is_refused(void) {
    Decision d = Sample();
    d.session_id = nullptr;
    AssertRefused(d);

    d = Sample();
    d.nonce = nullptr;
    AssertRefused(d);

    d = Sample();
    d.tool_name = nullptr;
    AssertRefused(d);

    d = Sample();
    d.input_sha256 = nullptr;
    AssertRefused(d);

    d = Sample();
    d.behavior = nullptr;
    AssertRefused(d);
}

// One loop over all four bounded fields, so a field added later cannot quietly
// skip the check — the shape `test_request_card.cpp` uses for the same reason.
//
// **Exactly one over, and exactly at, for each.** An oversized string of some
// round number would be refused by an off-by-one bound as well as by a correct
// one, and the boundary is the only part of a length check that is ever wrong.
void test_a_field_at_its_bound_is_taken_and_one_over_is_refused(void) {
    struct Field {
        const char *Decision::*member;
        size_t max;
    } fields[] = {
        {&Decision::session_id, protocol::kSessionIdMax},
        {&Decision::nonce, protocol::kNonceMax},
        {&Decision::tool_name, protocol::kToolNameMax},
        {&Decision::input_sha256, protocol::kSha256HexMax},
    };

    for (const auto &field : fields) {
        char text[128];
        std::memset(text, 'x', sizeof text);

        text[field.max] = '\0';
        Decision at = Sample();
        at.*field.member = text;
        char out[kSigningBytesMax];
        TEST_ASSERT_NOT_EQUAL(0, Assemble(at, out));

        text[field.max] = 'x';
        text[field.max + 1] = '\0';
        Decision over = Sample();
        over.*field.member = text;
        AssertRefused(over);
    }
}

// §7 has two behaviours. A third would be a verdict the protocol has no word
// for, and it must not become bytes somebody signs.
void test_a_behaviour_this_protocol_does_not_have_is_refused(void) {
    Decision d = Sample();
    d.behavior = "";
    AssertRefused(d);

    d.behavior = "ALLOW";
    AssertRefused(d);

    d.behavior = "allowed";
    AssertRefused(d);

    d.behavior = "skip";
    AssertRefused(d);
}

// A buffer one byte short of holding the message and its terminator. The
// interesting half is that it writes nothing at all rather than a truncated
// message — the same call §10.8.4 makes about a command that does not fit.
void test_a_buffer_that_is_too_small_is_refused_rather_than_filled(void) {
    char out[kSigningBytesMax];
    const size_t exact = Vector().signing_length;

    std::memset(out, kPoison, sizeof out);
    TEST_ASSERT_EQUAL_UINT32(0, protocol::DecisionSigningBytes(Sample(), out, exact));
    for (size_t i = 0; i < sizeof out; ++i) {
        TEST_ASSERT_EQUAL_CHAR(kPoison, out[i]);
    }

    // And one more byte is enough, which is what makes the line above a boundary
    // rather than a refusal that happens to be permanent.
    TEST_ASSERT_EQUAL_UINT32(exact, protocol::DecisionSigningBytes(Sample(), out, exact + 1));
}

// ---------------------------------------------------------------------------
// The integers on their own.

void test_append_int_spells_numbers_the_way_python_does(void) {
    struct Case {
        int64_t value;
        const char *text;
    } cases[] = {
        {0, "0"},
        {1, "1"},
        {-1, "-1"},
        {9, "9"},
        {10, "10"},
        {1737345600, "1737345600"},
        {INT64_MAX, "9223372036854775807"},
        // **The one with no positive counterpart.** Negating it is undefined,
        // which is what the obvious loop does; this is the value that catches it.
        {INT64_MIN, "-9223372036854775808"},
    };

    for (const auto &c : cases) {
        char out[32];
        std::memset(out, kPoison, sizeof out);
        const size_t n = protocol::AppendInt(c.value, out, sizeof out);
        TEST_ASSERT_EQUAL_UINT32(std::strlen(c.text), n);
        TEST_ASSERT_EQUAL_INT(0, std::memcmp(out, c.text, n));
    }
}

// No terminator is written, because this appends into the middle of a message.
void test_append_int_writes_only_digits(void) {
    char out[8];
    std::memset(out, kPoison, sizeof out);
    const size_t n = protocol::AppendInt(42, out, sizeof out);
    TEST_ASSERT_EQUAL_UINT32(2, n);
    TEST_ASSERT_EQUAL_CHAR(kPoison, out[n]);
}

void test_append_int_refuses_what_does_not_fit(void) {
    char out[32];

    // Exactly enough, then one short — and the short one writes nothing.
    TEST_ASSERT_EQUAL_UINT32(20, protocol::AppendInt(INT64_MIN, out, 20));

    std::memset(out, kPoison, sizeof out);
    TEST_ASSERT_EQUAL_UINT32(0, protocol::AppendInt(INT64_MIN, out, 19));
    TEST_ASSERT_EQUAL_CHAR(kPoison, out[0]);

    std::memset(out, kPoison, sizeof out);
    TEST_ASSERT_EQUAL_UINT32(0, protocol::AppendInt(-1, out, 1));
    TEST_ASSERT_EQUAL_CHAR(kPoison, out[0]);
}

// The bound in the header has to hold the worst case the fields allow, or the
// refusal above becomes reachable with a perfectly ordinary request.
void test_the_declared_bound_holds_every_field_at_its_limit(void) {
    char session[protocol::kSessionIdMax + 1];
    char nonce[protocol::kNonceMax + 1];
    char tool[protocol::kToolNameMax + 1];
    char sha[protocol::kSha256HexMax + 1];

    std::memset(session, 's', sizeof session);
    std::memset(nonce, 'n', sizeof nonce);
    std::memset(tool, 't', sizeof tool);
    std::memset(sha, 'a', sizeof sha);
    session[sizeof session - 1] = '\0';
    nonce[sizeof nonce - 1] = '\0';
    tool[sizeof tool - 1] = '\0';
    sha[sizeof sha - 1] = '\0';

    Decision d = Sample();
    d.v = INT32_MIN;
    d.ts = INT64_MIN;
    d.session_id = session;
    d.nonce = nonce;
    d.tool_name = tool;
    d.input_sha256 = sha;
    d.behavior = kBehaviorAllow;

    char out[kSigningBytesMax];
    TEST_ASSERT_NOT_EQUAL(0, Assemble(d, out));
}

}  // namespace

void RegisterSigningTests(void) {
    RUN_TEST(test_the_two_empty_fields_keep_their_positions);
    RUN_TEST(test_there_are_always_exactly_eight_separators);
    RUN_TEST(test_the_length_excludes_the_terminator_and_the_buffer_still_has_one);

    RUN_TEST(test_a_missing_field_is_refused);
    RUN_TEST(test_a_field_at_its_bound_is_taken_and_one_over_is_refused);
    RUN_TEST(test_a_behaviour_this_protocol_does_not_have_is_refused);
    RUN_TEST(test_a_buffer_that_is_too_small_is_refused_rather_than_filled);

    RUN_TEST(test_append_int_spells_numbers_the_way_python_does);
    RUN_TEST(test_append_int_writes_only_digits);
    RUN_TEST(test_append_int_refuses_what_does_not_fit);
    RUN_TEST(test_the_declared_bound_holds_every_field_at_its_limit);
}
