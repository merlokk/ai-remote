// The limits screen (CLAUDE.md §10.11, host tier; §9.2, §9.7, §10.8.3).
//
// Two subjects, and they are here together because they are the two halves of one
// screen: `protocol::ParseStatus`, which turns §9.7's document into fields, and
// `ui::LimitsView`, which decides what is on the glass and for how long.
//
// The document below is **§9.7's own example**, from `statusline/CLAUDE.md`, with
// the values it publishes. A fixture written to match the parser would pass for a
// parser that agrees with this file and with nothing else.
//
// What is pinned:
//
//   * **the two traffic-light scales of §9.2**, both boundaries of each, and the
//     assertion that they cannot drift into each other. Three implementations of
//     these numbers now exist — `render.rs`, `statusline.ts` and this;
//   * **`countdown()` against `render.rs`'s own cases**: `now`, `<1m`, `59m`,
//     `1h59m`, `4d8h`, and a reset in the past reading `now` rather than
//     underflowing;
//   * **absent is absent** — §9.7 omits a whole section for an API key rather than
//     a subscription, so a document with no `rate_limits` is ordinary and a gauge
//     drawn at 0 % would say the opposite of what is true;
//   * **junk keeps the last good document**, which is the opposite of the approval
//     path and the reason this file's refusals assert what is *still there*;
//   * **the arrival rules the owner asked for**: a document raises the screen, a
//     minute of silence drops it, and `PWR` dismisses the burst rather than one
//     message — see `limits_view.h` for why the last one cannot be literal.

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "limits_view.h"
#include "status.h"
#include "unity.h"

using protocol::ParseStatus;
using ui::Gauge;
using ui::Level;
using ui::Limits;
using ui::LimitsView;

namespace {

// §9.7's example document, verbatim.
constexpr char kDocument[] =
    "{\"ts\": 1786136782, \"line\": \"Opus 5 (1M context)\", \"session_id\": \"7b463c0f-aaaa\", "
    "\"cwd\": \"E:\\\\projects\\\\ai-remote\", \"model\": {\"id\": \"claude-opus-5[1m]\", "
    "\"display_name\": \"Opus 5 (1M context)\"}, \"effort\": {\"level\": \"high\"}, "
    "\"rate_limits\": {\"five_hour\": {\"used_percentage\": 65.0, \"resets_at\": 1786141200, "
    "\"resets_in\": 4418, \"resets_in_text\": \"1h13m\"}, \"seven_day\": {\"used_percentage\": "
    "27.0, \"resets_at\": 1786510800, \"resets_in\": 374018, \"resets_in_text\": \"4d7h\"}}, "
    "\"context_window\": {\"used_percentage\": 12.0}}";

// What §9.7 says a document with no subscription behind it looks like.
constexpr char kNoLimits[] = "{\"ts\":1786136782,\"line\":\"? | limits n/a\"}";

bool Parse(const char *json, Limits *out) { return ParseStatus(json, std::strlen(json), out); }

Limits Sample() {
    Limits limits;
    Parse(kDocument, &limits);
    return limits;
}

// ---------------------------------------------------------------------------
// The document.

void test_a_real_status_document_is_read(void) {
    Limits limits;
    TEST_ASSERT_TRUE(Parse(kDocument, &limits));

    TEST_ASSERT_EQUAL_INT64(1786136782, limits.ts);
    TEST_ASSERT_EQUAL_STRING("Opus 5 (1M context)", limits.model);
    TEST_ASSERT_EQUAL_STRING("high", limits.effort);
    TEST_ASSERT_EQUAL_STRING("E:\\projects\\ai-remote", limits.cwd);

    TEST_ASSERT_TRUE(limits.five_hour.present);
    TEST_ASSERT_EQUAL_UINT8(65, limits.five_hour.used_percent);
    TEST_ASSERT_EQUAL_INT64(1786141200, limits.five_hour.resets_at);
    TEST_ASSERT_EQUAL_INT32(4418, limits.five_hour.resets_in);
    TEST_ASSERT_EQUAL_STRING("1h13m", limits.five_hour.resets_in_text);

    TEST_ASSERT_TRUE(limits.seven_day.present);
    TEST_ASSERT_EQUAL_UINT8(27, limits.seven_day.used_percent);

    // The context window is a **sibling** of `rate_limits`, not a member — the
    // payload shape §9.7 publishes rather than the shape the line renders.
    TEST_ASSERT_TRUE(limits.context.present);
    TEST_ASSERT_EQUAL_UINT8(12, limits.context.used_percent);
    TEST_ASSERT_EQUAL_INT64(0, limits.context.resets_at);
}

// §9.7: "Absent is absent. Missing sections are omitted, never null." A gauge
// drawn at 0 % would read as a fresh window, which is the opposite of the truth.
void test_a_document_with_no_limits_is_ordinary(void) {
    Limits limits;
    TEST_ASSERT_TRUE(Parse(kNoLimits, &limits));
    TEST_ASSERT_FALSE(limits.five_hour.present);
    TEST_ASSERT_FALSE(limits.seven_day.present);
    TEST_ASSERT_FALSE(limits.context.present);
    TEST_ASSERT_EQUAL_INT64(1786136782, limits.ts);
}

// Each window is omitted on its own, so one without the other is a document to
// read rather than one to drop.
void test_one_window_without_the_other_is_read(void) {
    Limits limits;
    TEST_ASSERT_TRUE(Parse("{\"ts\":1,\"rate_limits\":{\"five_hour\":{\"used_percentage\":10}}}",
                           &limits));
    TEST_ASSERT_TRUE(limits.five_hour.present);
    TEST_ASSERT_FALSE(limits.seven_day.present);
}

// `ts` is the one required field: §9.7 calls it the clock the countdowns were
// resolved against, and it is the cheapest way to tell a status document from
// whatever else somebody publishes on an open subject.
void test_a_document_without_a_timestamp_is_not_one(void) {
    Limits limits;
    TEST_ASSERT_FALSE(Parse("{\"line\":\"hello\"}", &limits));
    TEST_ASSERT_FALSE(Parse("not json", &limits));
    TEST_ASSERT_FALSE(Parse("[1,2,3]", &limits));
    TEST_ASSERT_FALSE(Parse("42", &limits));
    TEST_ASSERT_FALSE(Parse("{\"ts\":\"soon\"}", &limits));
}

// The refusals leave the caller's struct alone, because the caller is holding the
// last good document and §10.8.3 says junk keeps it.
void test_a_refusal_keeps_the_last_good_document(void) {
    Limits limits = Sample();
    TEST_ASSERT_FALSE(Parse("{\"line\":\"junk\"}", &limits));
    TEST_ASSERT_EQUAL_STRING("Opus 5 (1M context)", limits.model);
    TEST_ASSERT_EQUAL_UINT8(65, limits.five_hour.used_percent);
}

// Clamped a second time after the publisher already did it: a bar drawn from
// somebody else's 130 overflows its track (§10.8.3).
void test_a_percentage_outside_the_scale_is_clamped(void) {
    Limits limits;
    TEST_ASSERT_TRUE(
        Parse("{\"ts\":1,\"context_window\":{\"used_percentage\":130}}", &limits));
    TEST_ASSERT_EQUAL_UINT8(100, limits.context.used_percent);

    TEST_ASSERT_TRUE(Parse("{\"ts\":1,\"context_window\":{\"used_percentage\":-5}}", &limits));
    TEST_ASSERT_EQUAL_UINT8(0, limits.context.used_percent);

    // Rounded rather than truncated — the publisher sends a float because it has
    // the resolution, and 65.7 % is nearer 66 than 65.
    TEST_ASSERT_TRUE(Parse("{\"ts\":1,\"context_window\":{\"used_percentage\":65.7}}", &limits));
    TEST_ASSERT_EQUAL_UINT8(66, limits.context.used_percent);
}

// **Truncated rather than refused, and this is the one place in the firmware
// where that is the right way round.** §10.8.4 refuses a `tool_input` that does
// not fit because a shortened command is one somebody approves by reflex; a
// shortened model name is a readout that is slightly less specific, and dropping
// the document over it would lose the numbers too.
void test_a_field_too_long_is_shortened_and_the_document_kept(void) {
    char json[512];
    char name[ui::kModelNameSize + 40];
    std::memset(name, 'm', sizeof name);
    name[sizeof name - 1] = '\0';
    std::snprintf(json, sizeof json,
                  "{\"ts\":7,\"model\":{\"display_name\":\"%s\"},"
                  "\"context_window\":{\"used_percentage\":50}}",
                  name);

    Limits limits;
    TEST_ASSERT_TRUE(Parse(json, &limits));
    TEST_ASSERT_EQUAL_UINT32(ui::kModelNameSize - 1, std::strlen(limits.model));
    TEST_ASSERT_EQUAL_UINT8(50, limits.context.used_percent);
}

// ---------------------------------------------------------------------------
// §9.2's traffic light.

void test_the_window_scale_is_the_one_render_rs_uses(void) {
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Level::kGreen), static_cast<int>(ui::WindowLevel(0)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Level::kGreen), static_cast<int>(ui::WindowLevel(50)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Level::kYellow), static_cast<int>(ui::WindowLevel(51)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Level::kYellow), static_cast<int>(ui::WindowLevel(80)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Level::kRed), static_cast<int>(ui::WindowLevel(81)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Level::kRed), static_cast<int>(ui::WindowLevel(100)));
}

void test_the_context_scale_is_the_one_render_rs_uses(void) {
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Level::kGreen), static_cast<int>(ui::ContextLevel(0)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Level::kGreen), static_cast<int>(ui::ContextLevel(20)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Level::kYellow), static_cast<int>(ui::ContextLevel(21)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Level::kYellow), static_cast<int>(ui::ContextLevel(45)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Level::kRed), static_cast<int>(ui::ContextLevel(46)));
}

// **The assertion §10.8.3 asks for by name**: the two scales must not drift into
// each other. Half a five-hour window is an ordinary working state; half a
// context window is most of the way to a compact.
void test_the_two_scales_cannot_drift_into_each_other(void) {
    bool differ = false;
    for (uint8_t used = 0; used <= 100; ++used) {
        if (ui::WindowLevel(used) != ui::ContextLevel(used)) {
            differ = true;
        }
        // The context scale is never more relaxed than the window one, at any
        // percentage — which is the direction the difference has to run.
        TEST_ASSERT_TRUE(static_cast<int>(ui::ContextLevel(used)) >=
                         static_cast<int>(ui::WindowLevel(used)));
    }
    TEST_ASSERT_TRUE_MESSAGE(differ, "the two scales have become the same scale");
}

// ---------------------------------------------------------------------------
// The countdown, against `render.rs`'s own cases.

void test_the_countdown_matches_the_rust_one(void) {
    struct Case {
        int64_t seconds;
        const char *text;
    } cases[] = {
        {0, "now"},        {-5, "now"},      {1, "<1m"},        {59, "<1m"},
        {60, "1m"},        {3599, "59m"},    {3600, "1h0m"},    {7140, "1h59m"},
        {86399, "23h59m"}, {86400, "1d0h"},  {374400, "4d8h"},
    };
    for (const Case &c : cases) {
        char out[ui::kResetsTextSize];
        ui::Countdown(c.seconds, out, sizeof out);
        TEST_ASSERT_EQUAL_STRING(c.text, out);
    }
}

// ---------------------------------------------------------------------------
// The arrival rules the owner asked for.

void test_a_document_raises_the_screen(void) {
    LimitsView view;
    TEST_ASSERT_FALSE(view.HasDocument());
    TEST_ASSERT_TRUE(view.Quiet());

    TEST_ASSERT_TRUE(view.Arrived(Sample(), 1000));
    TEST_ASSERT_TRUE(view.HasDocument());
    TEST_ASSERT_FALSE(view.Quiet());
    TEST_ASSERT_EQUAL_UINT32(1, view.Received());
}

// A minute with nothing, and the screen goes. Exactly at the boundary, so a
// changed constant is a failure rather than a shrug.
void test_a_minute_of_silence_takes_the_screen_away(void) {
    LimitsView view;
    view.Arrived(Sample(), 1000);

    TEST_ASSERT_FALSE(view.Tick(1000 + LimitsView::kQuietMs - 1));
    TEST_ASSERT_FALSE(view.Quiet());

    TEST_ASSERT_TRUE(view.Tick(1000 + LimitsView::kQuietMs));
    TEST_ASSERT_TRUE(view.Quiet());

    // Once only: the screen has already left, and a second cue would be a second
    // navigation.
    TEST_ASSERT_FALSE(view.Tick(1000 + LimitsView::kQuietMs + 5000));
}

// Documents keep arriving, so the minute keeps being pushed out. This is the
// ordinary state of a working session.
void test_a_stream_that_keeps_arriving_keeps_the_screen(void) {
    LimitsView view;
    uint32_t now = 1000;
    view.Arrived(Sample(), now);
    for (int i = 0; i < 10; ++i) {
        now += LimitsView::kQuietMs / 2;
        TEST_ASSERT_FALSE(view.Tick(now));
        view.Arrived(Sample(), now);
    }
    TEST_ASSERT_FALSE(view.Quiet());
}

// **The rule that could not be taken literally.** §9.7 publishes on every render,
// so "back until the next document" would be undone within seconds. A dismissal
// lasts the burst.
void test_a_dismissal_survives_the_documents_that_keep_coming(void) {
    LimitsView view;
    uint32_t now = 1000;
    view.Arrived(Sample(), now);
    view.Dismissed();
    TEST_ASSERT_TRUE(view.DismissedNow());

    // Ten more documents, none of which raises the screen.
    for (int i = 0; i < 10; ++i) {
        now += 2000;
        TEST_ASSERT_FALSE(view.Arrived(Sample(), now));
    }
}

// …and is spent the moment the stream stops, so the next session brings it back.
void test_a_dismissal_ends_when_the_stream_does(void) {
    LimitsView view;
    view.Arrived(Sample(), 1000);
    view.Dismissed();

    TEST_ASSERT_TRUE(view.Tick(1000 + LimitsView::kQuietMs));
    TEST_ASSERT_FALSE(view.DismissedNow());

    TEST_ASSERT_TRUE(view.Arrived(Sample(), 200000));
}

// The age is what the screen shows, and it is what makes "these numbers are as
// true as they are recent" visible (§9.7: a current value with no stream).
void test_the_age_is_the_time_since_it_landed(void) {
    LimitsView view;
    view.Arrived(Sample(), 5000);
    TEST_ASSERT_EQUAL_UINT32(0, view.AgeMs(5000));
    TEST_ASSERT_EQUAL_UINT32(12000, view.AgeMs(17000));
}

// The ~49-day millisecond wrap, the same case every timed thing in this firmware
// is asked about.
void test_the_quiet_window_survives_the_millisecond_wrap(void) {
    LimitsView view;
    const uint32_t before = 0xFFFFFFFFu - 1000;
    view.Arrived(Sample(), before);

    TEST_ASSERT_FALSE(view.Tick(before + LimitsView::kQuietMs - 1));
    TEST_ASSERT_TRUE(view.Tick(before + LimitsView::kQuietMs));
}

// ---------------------------------------------------------------------------
// Which clock the countdown comes from (§10.8.3).

void test_the_countdown_is_computed_when_the_clock_is_believable(void) {
    LimitsView view;
    view.Arrived(Sample(), 1000);

    // Two minutes before the five-hour window resets.
    const int64_t epoch = 1786141200 - 120;
    char out[ui::kResetsTextSize];
    view.CountdownFor(view.Document().five_hour, epoch, 1000, out, sizeof out);
    TEST_ASSERT_EQUAL_STRING("2m", out);
}

// **Before SNTP, the publisher's own resolution, aged by the time since it
// arrived** — a device whose clock is wrong by hours would otherwise print a
// countdown wrong by hours with nothing to say it was.
void test_the_countdown_falls_back_to_what_the_publisher_resolved(void) {
    LimitsView view;
    view.Arrived(Sample(), 1000);

    char out[ui::kResetsTextSize];
    view.CountdownFor(view.Document().five_hour, 0, 1000, out, sizeof out);
    TEST_ASSERT_EQUAL_STRING("1h13m", out);  // 4418 s, exactly what §9.7 published

    // …and it still ticks down: half an hour later it is half an hour shorter.
    view.CountdownFor(view.Document().five_hour, 0, 1000 + 1800000, out, sizeof out);
    TEST_ASSERT_EQUAL_STRING("43m", out);
}

// A gauge with no reset on a clock — the context window — has no countdown, and
// an empty string rather than `now`, which would be one that has run out.
void test_a_gauge_with_no_reset_has_no_countdown(void) {
    LimitsView view;
    view.Arrived(Sample(), 1000);

    char out[ui::kResetsTextSize];
    view.CountdownFor(view.Document().context, 1786136782, 1000, out, sizeof out);
    TEST_ASSERT_EQUAL_STRING("", out);

    Gauge absent;
    view.CountdownFor(absent, 1786136782, 1000, out, sizeof out);
    TEST_ASSERT_EQUAL_STRING("", out);
}

}  // namespace

void RegisterLimitsTests(void) {
    RUN_TEST(test_a_real_status_document_is_read);
    RUN_TEST(test_a_document_with_no_limits_is_ordinary);
    RUN_TEST(test_one_window_without_the_other_is_read);
    RUN_TEST(test_a_document_without_a_timestamp_is_not_one);
    RUN_TEST(test_a_refusal_keeps_the_last_good_document);
    RUN_TEST(test_a_percentage_outside_the_scale_is_clamped);
    RUN_TEST(test_a_field_too_long_is_shortened_and_the_document_kept);

    RUN_TEST(test_the_window_scale_is_the_one_render_rs_uses);
    RUN_TEST(test_the_context_scale_is_the_one_render_rs_uses);
    RUN_TEST(test_the_two_scales_cannot_drift_into_each_other);
    RUN_TEST(test_the_countdown_matches_the_rust_one);

    RUN_TEST(test_a_document_raises_the_screen);
    RUN_TEST(test_a_minute_of_silence_takes_the_screen_away);
    RUN_TEST(test_a_stream_that_keeps_arriving_keeps_the_screen);
    RUN_TEST(test_a_dismissal_survives_the_documents_that_keep_coming);
    RUN_TEST(test_a_dismissal_ends_when_the_stream_does);
    RUN_TEST(test_the_age_is_the_time_since_it_landed);
    RUN_TEST(test_the_quiet_window_survives_the_millisecond_wrap);

    RUN_TEST(test_the_countdown_is_computed_when_the_clock_is_believable);
    RUN_TEST(test_the_countdown_falls_back_to_what_the_publisher_resolved);
    RUN_TEST(test_a_gauge_with_no_reset_has_no_countdown);
}
