// The request card of CLAUDE.md §10.8.4, tested where every rule in it is about
// not approving something by accident (§10.11, host tier).
//
// `request_card.h` includes `<cstdint>`, `<cstddef>` and the navigator, so this
// suite needs no fake — the navigator's shape rather than the drivers'.
//
// Five blocks, and the order is the order the rules matter in:
//
//   * what a card is allowed to be — every refusal, because a refused card is
//     §10.10's fail-safe and a *shown* card is a question put to a human;
//   * the queue, and that the oldest is the one on screen;
//   * the press guard, which is the whole of §10.8.1's queued touch;
//   * the outcomes — and the assertion that no amount of time produces one;
//   * the receipt, which exists so that a request nobody answered is visible.

#include <cstring>

#include "request_card.h"
#include "unity.h"

using ui::CardState;
using ui::Navigator;
using ui::Outcome;
using ui::Request;
using ui::RequestCard;
using ui::Verdict;

namespace {

// A card that is allowed to be shown, so a test about anything else does not
// have to build one.
Request Sane() {
    Request request;
    request.v = 1;
    request.ts = 1786924800;
    std::strcpy(request.session_id, "5d43590e-8d41-4b83-b653-a3d7e00a566c");
    std::strcpy(request.nonce, "3q2+7wAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=");
    std::strcpy(request.input_sha256,
                "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    std::strcpy(request.tool_name, "Bash");
    std::strcpy(request.cwd, "E:\\projects\\ai-remote");
    std::strcpy(request.tool_input, "{\"command\": \"rm -rf build\"}");
    std::strcpy(request.reply, "_INBOX.9f2c1d4e5a6b7c8d9e0f1a2b3c4d5e6f");
    request.ttl_ms = 30000;
    return request;
}

Request Named(const char *tool) {
    Request request = Sane();
    std::strcpy(request.tool_name, tool);
    return request;
}

// The tool on the card, or a name that fails the comparison rather than the
// process. A mutation pass earned this: a broken queue rule makes `Front()` null,
// and a test that dereferences it dies without printing which test it was.
const char *FrontTool(const RequestCard &card) {
    const ui::Request *front = card.Front();
    return front != nullptr ? front->tool_name : "<no card>";
}

// --- What a card is allowed to be ---------------------------------------

void test_nothing_pending_is_nothing_on_screen(void) {
    RequestCard card;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(CardState::kIdle), static_cast<int>(card.State()));
    TEST_ASSERT_NULL(card.Front());
    TEST_ASSERT_EQUAL_UINT8(0, card.Pending());
    TEST_ASSERT_EQUAL_UINT8(0, card.Waiting());
    TEST_ASSERT_EQUAL_UINT32(0, card.RemainingMs(1000));
}

void test_a_request_becomes_the_card(void) {
    RequestCard card;
    TEST_ASSERT_TRUE(card.Arrived(Sane(), 1000));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(CardState::kCard), static_cast<int>(card.State()));
    TEST_ASSERT_NOT_NULL(card.Front());
    TEST_ASSERT_EQUAL_STRING("Bash", FrontTool(card));
    TEST_ASSERT_EQUAL_UINT8(1, card.Pending());
    TEST_ASSERT_EQUAL_UINT8(0, card.Waiting());
}

void test_a_tool_input_that_does_not_fit_is_refused_not_shown(void) {
    // §10.8.4: never truncated into something that reads as harmless. A command
    // the operator cannot see all of is not a question they can answer, so there
    // is no card — and §10.10's silence is what the hook falls back from.
    RequestCard card;
    Request request = Sane();
    std::memset(request.tool_input, 'x', sizeof(request.tool_input));  // no terminator

    TEST_ASSERT_FALSE(card.Arrived(request, 1000));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(CardState::kIdle), static_cast<int>(card.State()));
    TEST_ASSERT_EQUAL_UINT16(1, card.Refused());
}

void test_every_text_field_is_refused_the_same_way(void) {
    // One rule at the door, so a field added later cannot quietly skip it.
    RequestCard card;
    Request base = Sane();

    struct Field {
        char *at;
        size_t size;
    };
    const Field fields[] = {
        {base.session_id, ui::kSessionIdSize},   {base.nonce, ui::kNonceSize},
        {base.input_sha256, ui::kSha256HexSize}, {base.tool_name, ui::kToolNameSize},
        {base.cwd, ui::kCwdSize},                {base.tool_input, ui::kToolInputSize},
        {base.reply, ui::kReplySubjectSize},
    };

    for (const Field &field : fields) {
        Request request = Sane();
        // The same offset in the fresh copy, filled to the brim with no NUL.
        const size_t offset = static_cast<size_t>(field.at - reinterpret_cast<char *>(&base));
        std::memset(reinterpret_cast<char *>(&request) + offset, 'x', field.size);
        TEST_ASSERT_FALSE(card.Arrived(request, 1000));
        TEST_ASSERT_EQUAL_INT(static_cast<int>(CardState::kIdle), static_cast<int>(card.State()));
    }
    TEST_ASSERT_EQUAL_UINT16(sizeof(fields) / sizeof(fields[0]), card.Refused());
}

void test_a_request_with_nowhere_to_answer_is_not_a_card(void) {
    // §10.10: never publish into a dead inbox. An empty reply subject is the
    // deadest of them, and a card whose press could not reach anybody is worse
    // than no card at all.
    RequestCard card;
    Request request = Sane();
    request.reply[0] = '\0';
    TEST_ASSERT_FALSE(card.Arrived(request, 1000));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(CardState::kIdle), static_cast<int>(card.State()));
}

void test_a_request_with_no_tool_is_not_a_card(void) {
    // The tool name is the question. Without it the screen would be asking about
    // nothing and the operator would have to guess from the arguments.
    RequestCard card;
    Request request = Sane();
    request.tool_name[0] = '\0';
    TEST_ASSERT_FALSE(card.Arrived(request, 1000));
}

// --- The queue -----------------------------------------------------------

void test_the_oldest_is_the_one_on_screen(void) {
    RequestCard card;
    TEST_ASSERT_TRUE(card.Arrived(Named("Bash"), 1000));
    TEST_ASSERT_TRUE(card.Arrived(Named("Write"), 1100));

    TEST_ASSERT_EQUAL_STRING("Bash", FrontTool(card));
    TEST_ASSERT_EQUAL_UINT8(2, card.Pending());
    TEST_ASSERT_EQUAL_UINT8(1, card.Waiting());
}

void test_the_queue_is_bounded_and_the_bound_is_the_navigators(void) {
    // Tied rather than repeated: a queue that could hold five while the navigator
    // counted four would put a card on screen with no navigation behind it.
    TEST_ASSERT_EQUAL_UINT8(Navigator::kMaxPending, RequestCard::kMaxPending);

    RequestCard card;
    for (uint8_t i = 0; i < RequestCard::kMaxPending; ++i) {
        TEST_ASSERT_TRUE(card.Arrived(Sane(), 1000 + i));
    }
    TEST_ASSERT_FALSE(card.Arrived(Sane(), 2000));
    TEST_ASSERT_EQUAL_UINT8(RequestCard::kMaxPending, card.Pending());
    TEST_ASSERT_EQUAL_UINT16(1, card.Refused());
}

void test_room_freed_is_room_usable(void) {
    RequestCard card;
    for (uint8_t i = 0; i < RequestCard::kMaxPending; ++i) {
        TEST_ASSERT_TRUE(card.Arrived(Sane(), 1000));
    }
    Request answered;
    TEST_ASSERT_TRUE(card.Press(Verdict::kDeny, 1000 + RequestCard::kPressGuardMs, &answered));
    TEST_ASSERT_TRUE(card.Arrived(Sane(), 3000));
    TEST_ASSERT_EQUAL_UINT8(RequestCard::kMaxPending, card.Pending());
}

// --- The press guard (§10.8.1) -------------------------------------------

void test_a_press_inside_the_guard_is_thrown_away(void) {
    RequestCard card;
    card.Arrived(Sane(), 1000);

    Request answered;
    TEST_ASSERT_FALSE(card.Press(Verdict::kAllow, 1000, &answered));
    TEST_ASSERT_FALSE(
        card.Press(Verdict::kAllow, 1000 + RequestCard::kPressGuardMs - 1, &answered));
    TEST_ASSERT_EQUAL_UINT16(2, card.Ignored());
    TEST_ASSERT_EQUAL_UINT16(0, card.Allowed());

    // …and the card is still there to be answered properly.
    TEST_ASSERT_TRUE(card.Press(Verdict::kAllow, 1000 + RequestCard::kPressGuardMs, &answered));
    TEST_ASSERT_EQUAL_UINT16(1, card.Allowed());
}

void test_a_finger_already_down_when_the_card_arrived_is_not_a_press(void) {
    // The other half of §10.8.1's rule, and the same comparison: a press that
    // began before the card appeared has a `pressed_at` earlier than the card's
    // own, which is negative and therefore never past the guard.
    RequestCard card;
    card.Arrived(Sane(), 5000);

    Request answered;
    TEST_ASSERT_FALSE(card.Press(Verdict::kAllow, 4900, &answered));
    TEST_ASSERT_FALSE(card.Press(Verdict::kAllow, 1, &answered));
    TEST_ASSERT_EQUAL_UINT16(2, card.Ignored());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(CardState::kCard), static_cast<int>(card.State()));
}

void test_the_next_card_gets_its_own_guard(void) {
    // §10.8.4: answering one brings the next up instantly, "which is precisely
    // when the 300 ms guard earns its place" — a second press following the first
    // by a few milliseconds must not answer a card the operator has not read.
    RequestCard card;
    card.Arrived(Named("Bash"), 1000);
    card.Arrived(Named("Write"), 1000);

    Request answered;
    const uint32_t first = 1000 + RequestCard::kPressGuardMs;
    TEST_ASSERT_TRUE(card.Press(Verdict::kAllow, first, &answered));
    TEST_ASSERT_EQUAL_STRING("Bash", answered.tool_name);
    TEST_ASSERT_EQUAL_STRING("Write", FrontTool(card));

    TEST_ASSERT_FALSE(card.Press(Verdict::kAllow, first + 10, &answered));
    TEST_ASSERT_TRUE(card.Press(Verdict::kAllow, first + RequestCard::kPressGuardMs, &answered));
    TEST_ASSERT_EQUAL_STRING("Write", answered.tool_name);
}

void test_pressing_nothing_decides_nothing(void) {
    RequestCard card;
    Request answered;
    TEST_ASSERT_FALSE(card.Press(Verdict::kAllow, 100000, &answered));
    TEST_ASSERT_EQUAL_UINT16(0, card.Allowed());
    TEST_ASSERT_EQUAL_UINT16(0, card.Ignored());
}

// --- The outcomes --------------------------------------------------------

void test_a_press_hands_back_the_request_it_is_about(void) {
    // The reply has to echo §7's fields, so the decision carries the whole
    // request rather than a reference into a slot the next card is about to own.
    RequestCard card;
    card.Arrived(Sane(), 1000);

    Request answered;
    std::memset(&answered, 0, sizeof(answered));
    TEST_ASSERT_TRUE(card.Press(Verdict::kDeny, 1000 + RequestCard::kPressGuardMs, &answered));

    TEST_ASSERT_EQUAL_INT32(1, answered.v);
    TEST_ASSERT_EQUAL_STRING("Bash", answered.tool_name);
    TEST_ASSERT_EQUAL_STRING("_INBOX.9f2c1d4e5a6b7c8d9e0f1a2b3c4d5e6f", answered.reply);
    TEST_ASSERT_EQUAL_STRING("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
                             answered.input_sha256);
    TEST_ASSERT_EQUAL_UINT16(1, card.Denied());
    TEST_ASSERT_EQUAL_UINT16(0, card.Allowed());
}

void test_no_amount_of_time_answers_a_card(void) {
    // §10.10, and the single most important assertion in this file: `Tick` runs
    // the timers, and a timer is not a human. Two minutes of it against a card
    // with a one-minute life produces an expiry and **never** a verdict.
    RequestCard card;
    card.Arrived(Sane(), 0);

    for (uint32_t now = 0; now < 120000; now += 20) {
        card.Tick(now);
    }
    TEST_ASSERT_EQUAL_UINT16(0, card.Allowed());
    TEST_ASSERT_EQUAL_UINT16(0, card.Denied());
    TEST_ASSERT_EQUAL_UINT16(1, card.TimedOut());
}

void test_a_card_that_timed_out_reads_as_a_timeout_and_not_as_a_deny(void) {
    // The hook has already fallen back to its own prompt. An operator who saw
    // "denied" would believe they had answered something they never saw.
    RequestCard card;
    Request request = Sane();
    request.ttl_ms = 5000;
    card.Arrived(request, 1000);

    TEST_ASSERT_FALSE(card.Tick(5999));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(CardState::kCard), static_cast<int>(card.State()));

    TEST_ASSERT_TRUE(card.Tick(6000));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(CardState::kReceipt), static_cast<int>(card.State()));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Outcome::kTimedOut),
                          static_cast<int>(card.LastOutcome()));
    TEST_ASSERT_EQUAL_STRING("Bash", card.LastTool());
    TEST_ASSERT_EQUAL_UINT16(0, card.Denied());
}

void test_a_request_waiting_past_its_own_life_is_never_shown(void) {
    // It sat behind another card until the hook stopped waiting for it. Bringing
    // it up would be putting a question on screen whose answer could only be
    // published into a dead inbox.
    RequestCard card;
    Request first = Sane();
    first.ttl_ms = 60000;
    std::strcpy(first.tool_name, "Bash");
    card.Arrived(first, 1000);

    Request second = Named("Write");
    second.ttl_ms = 5000;
    card.Arrived(second, 1000);

    TEST_ASSERT_TRUE(card.Tick(6000));
    TEST_ASSERT_EQUAL_UINT8(1, card.Pending());
    TEST_ASSERT_EQUAL_STRING("Bash", FrontTool(card));
    TEST_ASSERT_EQUAL_UINT16(1, card.TimedOut());

    // And the card that is up was not disturbed by it.
    TEST_ASSERT_EQUAL_INT(static_cast<int>(CardState::kCard), static_cast<int>(card.State()));
}

void test_a_missing_ttl_falls_back_to_one_that_expires(void) {
    // Zero must not mean "forever": a card that outlives every hook waiting for
    // it is a card whose press publishes into a dead inbox.
    RequestCard card;
    Request request = Sane();
    request.ttl_ms = 0;
    card.Arrived(request, 0);

    TEST_ASSERT_EQUAL_UINT32(RequestCard::kDefaultTtlMs, card.RemainingMs(0));
    card.Tick(RequestCard::kDefaultTtlMs);
    TEST_ASSERT_EQUAL_UINT16(1, card.TimedOut());
}

void test_the_countdown_counts_down_and_floors_at_zero(void) {
    RequestCard card;
    Request request = Sane();
    request.ttl_ms = 10000;
    card.Arrived(request, 1000);

    TEST_ASSERT_EQUAL_UINT32(10000, card.RemainingMs(1000));
    TEST_ASSERT_EQUAL_UINT32(4000, card.RemainingMs(7000));
    TEST_ASSERT_EQUAL_UINT32(0, card.RemainingMs(11000));
    TEST_ASSERT_EQUAL_UINT32(0, card.RemainingMs(99000));
}

void test_the_card_survives_the_millisecond_wrap(void) {
    RequestCard card;
    Request request = Sane();
    request.ttl_ms = 30000;
    const uint32_t arrived = 0xFFFFF000u;
    card.Arrived(request, arrived);

    // The deadline is past the wrap, so an unsigned `now >= deadline` would fire
    // at once and the card would vanish before anybody saw it.
    TEST_ASSERT_FALSE(card.Tick(arrived + 1000));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(CardState::kCard), static_cast<int>(card.State()));
    TEST_ASSERT_EQUAL_UINT32(29000, card.RemainingMs(arrived + 1000));

    Request answered;
    TEST_ASSERT_TRUE(card.Press(Verdict::kAllow, arrived + 1000, &answered));
    TEST_ASSERT_EQUAL_UINT16(1, card.Allowed());
}

// --- The receipt ---------------------------------------------------------

void test_answering_the_last_card_leaves_a_receipt_that_fades(void) {
    RequestCard card;
    card.Arrived(Sane(), 1000);

    Request answered;
    const uint32_t at = 1000 + RequestCard::kPressGuardMs;
    TEST_ASSERT_TRUE(card.Press(Verdict::kAllow, at, &answered));

    TEST_ASSERT_EQUAL_INT(static_cast<int>(CardState::kReceipt), static_cast<int>(card.State()));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Outcome::kAllowed), static_cast<int>(card.LastOutcome()));
    TEST_ASSERT_EQUAL_STRING("Bash", card.LastTool());
    TEST_ASSERT_NULL(card.Front());

    TEST_ASSERT_FALSE(card.Tick(at + RequestCard::kReceiptMs - 1));
    TEST_ASSERT_TRUE(card.Tick(at + RequestCard::kReceiptMs));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(CardState::kIdle), static_cast<int>(card.State()));
}

void test_answering_with_another_waiting_shows_no_receipt(void) {
    // A note about the request just finished must not sit in front of the one
    // still pending — §10.8.1's rule that nothing quiet steals the screen,
    // pointed the other way.
    RequestCard card;
    card.Arrived(Named("Bash"), 1000);
    card.Arrived(Named("Write"), 1000);

    Request answered;
    TEST_ASSERT_TRUE(card.Press(Verdict::kAllow, 1000 + RequestCard::kPressGuardMs, &answered));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(CardState::kCard), static_cast<int>(card.State()));
    TEST_ASSERT_EQUAL_STRING("Write", FrontTool(card));
}

void test_an_arriving_request_outranks_a_receipt(void) {
    RequestCard card;
    card.Arrived(Named("Bash"), 1000);

    Request answered;
    const uint32_t at = 1000 + RequestCard::kPressGuardMs;
    card.Press(Verdict::kDeny, at, &answered);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(CardState::kReceipt), static_cast<int>(card.State()));

    TEST_ASSERT_TRUE(card.Arrived(Named("Write"), at + 100));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(CardState::kCard), static_cast<int>(card.State()));
    TEST_ASSERT_EQUAL_STRING("Write", FrontTool(card));

    // And it gets a full guard from when it appeared, not from when the receipt
    // started.
    TEST_ASSERT_FALSE(card.Press(Verdict::kAllow, at + 100, &answered));
    TEST_ASSERT_TRUE(card.Press(Verdict::kAllow, at + 100 + RequestCard::kPressGuardMs, &answered));
}

}  // namespace

void RegisterRequestCardTests(void) {
    RUN_TEST(test_nothing_pending_is_nothing_on_screen);
    RUN_TEST(test_a_request_becomes_the_card);
    RUN_TEST(test_a_tool_input_that_does_not_fit_is_refused_not_shown);
    RUN_TEST(test_every_text_field_is_refused_the_same_way);
    RUN_TEST(test_a_request_with_nowhere_to_answer_is_not_a_card);
    RUN_TEST(test_a_request_with_no_tool_is_not_a_card);

    RUN_TEST(test_the_oldest_is_the_one_on_screen);
    RUN_TEST(test_the_queue_is_bounded_and_the_bound_is_the_navigators);
    RUN_TEST(test_room_freed_is_room_usable);

    RUN_TEST(test_a_press_inside_the_guard_is_thrown_away);
    RUN_TEST(test_a_finger_already_down_when_the_card_arrived_is_not_a_press);
    RUN_TEST(test_the_next_card_gets_its_own_guard);
    RUN_TEST(test_pressing_nothing_decides_nothing);

    RUN_TEST(test_a_press_hands_back_the_request_it_is_about);
    RUN_TEST(test_no_amount_of_time_answers_a_card);
    RUN_TEST(test_a_card_that_timed_out_reads_as_a_timeout_and_not_as_a_deny);
    RUN_TEST(test_a_request_waiting_past_its_own_life_is_never_shown);
    RUN_TEST(test_a_missing_ttl_falls_back_to_one_that_expires);
    RUN_TEST(test_the_countdown_counts_down_and_floors_at_zero);
    RUN_TEST(test_the_card_survives_the_millisecond_wrap);

    RUN_TEST(test_answering_the_last_card_leaves_a_receipt_that_fades);
    RUN_TEST(test_answering_with_another_waiting_shows_no_receipt);
    RUN_TEST(test_an_arriving_request_outranks_a_receipt);
}
