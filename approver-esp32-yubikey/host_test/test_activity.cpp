// The activity line (CLAUDE.md §10.11, host tier; §9.10, §10.8.3).
//
// The sibling of `test_limits.cpp`, and it is a separate file for the same reason
// the two documents are separate subjects: one is what a session is spending, the
// other what it is doing, and a suite that mixed them would make it harder to see
// which half a failure is in.
//
// The document below is **§9.10's own example**, from `statusline/CLAUDE.md`. A
// fixture written to match the parser would pass for a parser that agrees with
// this file and with nothing else.
//
// What is pinned:
//
//   * **`v` is the "this is ours" test**, not decoration: absent, wrong, or a
//     version from a newer publisher are all refusals, and a refusal leaves the
//     last good document standing;
//   * **an event or state this firmware does not know is refused** rather than
//     drawn as an unknown word — which is what lets everything downstream take an
//     enum;
//   * **`post_tool` is `thinking`, never `idle`** — the tool is done, the turn is
//     not, and only `stop` means "waiting for a human";
//   * **the headline is ASCII and never split mid-character.** The panel draws
//     Montserrat's default subset, so a middle dot would be a placeholder box, and
//     half a UTF-8 sequence would be another one;
//   * **`idle` never goes stale** while a running document does after ten minutes
//     — `activity_view.h` argues the asymmetry.

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "activity.h"
#include "activity_view.h"
#include "unity.h"

using protocol::ParseActivity;
using ui::Activity;
using ui::ActivityEvent;
using ui::ActivityState;
using ui::ActivityView;

namespace {

// §9.10's example document, verbatim.
constexpr char kPreTool[] =
    "{\"v\": 1, \"ts\": 1786136782, \"event\": \"pre_tool\", \"state\": \"running\", "
    "\"session_id\": \"7b463c0f-aaaa\", \"cwd\": \"E:\\\\projects\\\\ai-remote\", "
    "\"tool_name\": \"Bash\", \"summary\": \"py -m pytest -q\", "
    "\"tool_use_id\": \"toolu_01ABC123\", \"agent_type\": \"Explore\"}";

// What §9.10 says the smallest document looks like: a turn that ended.
constexpr char kStop[] = "{\"v\":1,\"ts\":1786136999,\"event\":\"stop\",\"state\":\"idle\"}";

bool Parse(const char *json, Activity *out) { return ParseActivity(json, std::strlen(json), out); }

Activity Sample() {
    Activity activity;
    Parse(kPreTool, &activity);
    return activity;
}

void HeadlineOf(const Activity &activity, char *out, size_t capacity) {
    ActivityView view;
    view.Arrived(activity, 1000);
    view.Headline(out, capacity);
}

// Fills `out` with `count` copies of `byte`, terminated — the tests below need
// strings longer than the fields they land in, and §10.14.1 keeps `std::string`
// for the one place that has to have it.
void Fill(char *out, size_t count, char byte) {
    std::memset(out, byte, count);
    out[count] = '\0';
}

// ---------------------------------------------------------------------------
// The document.

void test_a_real_activity_document_is_read(void) {
    Activity activity;
    TEST_ASSERT_TRUE(Parse(kPreTool, &activity));

    TEST_ASSERT_EQUAL_INT64(1786136782, activity.ts);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ActivityEvent::kPreTool),
                          static_cast<int>(activity.event));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ActivityState::kRunning),
                          static_cast<int>(activity.state));
    TEST_ASSERT_EQUAL_STRING("Bash", activity.tool);
    TEST_ASSERT_EQUAL_STRING("py -m pytest -q", activity.summary);
    TEST_ASSERT_EQUAL_STRING("Explore", activity.agent);
}

void test_a_turn_that_ended_is_idle_and_says_nothing_about_a_tool(void) {
    Activity activity;
    TEST_ASSERT_TRUE(Parse(kStop, &activity));

    TEST_ASSERT_EQUAL_INT(static_cast<int>(ActivityEvent::kStop), static_cast<int>(activity.event));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ActivityState::kIdle), static_cast<int>(activity.state));
    TEST_ASSERT_EQUAL_STRING("", activity.tool);
    TEST_ASSERT_EQUAL_STRING("", activity.summary);
    TEST_ASSERT_EQUAL_STRING("", activity.agent);
}

void test_a_tool_that_has_run_is_thinking_not_idle(void) {
    Activity activity;
    TEST_ASSERT_TRUE(Parse("{\"v\":1,\"ts\":42,\"event\":\"post_tool\",\"state\":\"thinking\","
                           "\"tool_name\":\"Edit\",\"summary\":\"main.cpp\"}",
                           &activity));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ActivityEvent::kPostTool),
                          static_cast<int>(activity.event));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ActivityState::kThinking),
                          static_cast<int>(activity.state));
}

void test_the_version_is_what_makes_a_document_ours(void) {
    // `activity` is as open as `status` and `approvals.*`, and unlike §9.7's
    // document there is no always-present pair of fields to recognise this one by.
    const char *refused[] = {
        "{\"ts\":42,\"event\":\"stop\",\"state\":\"idle\"}",              // no `v`
        "{\"v\":2,\"ts\":42,\"event\":\"stop\",\"state\":\"idle\"}",      // a newer publisher
        "{\"v\":\"1\",\"ts\":42,\"event\":\"stop\",\"state\":\"idle\"}",  // a string, not a number
        "{\"v\":1,\"event\":\"stop\",\"state\":\"idle\"}",                // no clock
        "{\"v\":1,\"ts\":42,\"state\":\"idle\"}",                         // no event
        "{\"v\":1,\"ts\":42,\"event\":\"stop\"}",                         // no state
    };
    for (const char *json : refused) {
        Activity activity = Sample();
        TEST_ASSERT_FALSE_MESSAGE(Parse(json, &activity), json);
        // **The last good document is still standing** — the opposite of the
        // approval path, and the same rule `ParseStatus` follows.
        TEST_ASSERT_EQUAL_STRING("Bash", activity.tool);
    }
}

void test_a_word_this_firmware_does_not_know_is_refused(void) {
    const char *refused[] = {
        "{\"v\":1,\"ts\":42,\"event\":\"compacting\",\"state\":\"idle\"}",
        "{\"v\":1,\"ts\":42,\"event\":\"stop\",\"state\":\"asleep\"}",
        "{\"v\":1,\"ts\":42,\"event\":\"Stop\",\"state\":\"idle\"}",  // case matters
        "{\"v\":1,\"ts\":42,\"event\":1,\"state\":\"idle\"}",
    };
    for (const char *json : refused) {
        Activity activity = Sample();
        TEST_ASSERT_FALSE_MESSAGE(Parse(json, &activity), json);
        TEST_ASSERT_EQUAL_STRING("Bash", activity.tool);
    }
}

void test_junk_on_the_subject_keeps_the_last_good_document(void) {
    Activity activity = Sample();
    TEST_ASSERT_FALSE(Parse("not json at all", &activity));
    TEST_ASSERT_FALSE(Parse("[1,2,3]", &activity));
    TEST_ASSERT_FALSE(Parse("\"hello\"", &activity));
    TEST_ASSERT_FALSE(ParseActivity(nullptr, 10, &activity));
    TEST_ASSERT_FALSE(ParseActivity(kPreTool, 0, &activity));
    TEST_ASSERT_EQUAL_STRING("Bash", activity.tool);
    TEST_ASSERT_EQUAL_INT64(1786136782, activity.ts);
}

void test_a_document_too_long_to_be_ours_is_dropped_unread(void) {
    // §10.10: a flood on an open subject must cost nothing, and the length is the
    // one thing that can be checked without parsing.
    char big[protocol::kMaxActivityBytes + 64];
    std::memset(big, 'x', sizeof big);
    Activity activity;
    TEST_ASSERT_FALSE(ParseActivity(big, sizeof big, &activity));
}

void test_text_longer_than_the_field_is_truncated_not_refused(void) {
    // The same call `ParseStatus` makes: a shortened readout is slightly less
    // specific, and refusing the document would lose the state as well.
    char tool[81];
    Fill(tool, sizeof tool - 1, 'T');
    char summary[401];
    Fill(summary, sizeof summary - 1, 'S');

    char json[600];
    std::snprintf(json, sizeof json,
                  "{\"v\":1,\"ts\":42,\"event\":\"pre_tool\",\"state\":\"running\","
                  "\"tool_name\":\"%s\",\"summary\":\"%s\"}",
                  tool, summary);

    Activity activity;
    TEST_ASSERT_TRUE(Parse(json, &activity));
    TEST_ASSERT_EQUAL_size_t(ui::kActivityToolSize - 1, std::strlen(activity.tool));
    TEST_ASSERT_EQUAL_size_t(ui::kActivitySummarySize - 1, std::strlen(activity.summary));
}

void test_truncation_never_splits_a_character(void) {
    // A path with Cyrillic in it: two bytes per character, so a byte-counted cut
    // lands mid-sequence every other character — and half a character is a
    // placeholder box on the panel.
    char cyrillic[401];
    size_t at = 0;
    while (at + 2 < sizeof cyrillic) {
        cyrillic[at++] = static_cast<char>(0xD1);  // U+0449, two bytes
        cyrillic[at++] = static_cast<char>(0x89);
    }
    cyrillic[at] = '\0';

    char json[900];
    std::snprintf(json, sizeof json,
                  "{\"v\":1,\"ts\":42,\"event\":\"pre_tool\",\"state\":\"running\","
                  "\"summary\":\"%s\"}",
                  cyrillic);

    Activity activity;
    TEST_ASSERT_TRUE(Parse(json, &activity));

    const size_t length = std::strlen(activity.summary);
    TEST_ASSERT_TRUE(length > 0);
    TEST_ASSERT_TRUE(length < ui::kActivitySummarySize);
    // Every byte is a lead byte or the continuation of one, in pairs — nothing
    // was cut between the two.
    TEST_ASSERT_EQUAL_size_t(0, length % 2);
    for (size_t byte = 0; byte < length; byte += 2) {
        TEST_ASSERT_EQUAL_UINT8(0xD1, static_cast<uint8_t>(activity.summary[byte]));
        TEST_ASSERT_EQUAL_UINT8(0x89, static_cast<uint8_t>(activity.summary[byte + 1]));
    }
}

// ---------------------------------------------------------------------------
// The line.

void test_the_headline_reads_in_the_order_it_is_worth_reading(void) {
    char line[ui::kActivityHeadlineSize];

    // The tool comes first because it is the part that is always there; the
    // summary is what makes the line worth reading.
    Activity activity = Sample();
    activity.agent[0] = '\0';
    HeadlineOf(activity, line, sizeof line);
    TEST_ASSERT_EQUAL_STRING("Bash - py -m pytest -q", line);

    // A subagent says whose work it is, ahead of both.
    HeadlineOf(Sample(), line, sizeof line);
    TEST_ASSERT_EQUAL_STRING("Explore > Bash - py -m pytest -q", line);

    // A tool with nothing worth quoting (`TodoWrite`) is its own name.
    activity = Sample();
    activity.summary[0] = '\0';
    activity.agent[0] = '\0';
    HeadlineOf(activity, line, sizeof line);
    TEST_ASSERT_EQUAL_STRING("Bash", line);

    // And a document with no tool at all is the state's own word.
    Activity stop;
    TEST_ASSERT_TRUE(Parse(kStop, &stop));
    HeadlineOf(stop, line, sizeof line);
    TEST_ASSERT_EQUAL_STRING("idle", line);
}

void test_only_a_running_tool_is_named(void) {
    char line[ui::kActivityHeadlineSize];

    // **A tool that has finished is not what the session is doing.** `post_tool`
    // carries the tool that just returned, and drawing it reads as "still running
    // Edit" — a line that cannot be told from a true one, which is the one thing
    // this readout must not be (§10.8.3). So the state's own word, and the tool it
    // arrived with is dropped rather than drawn.
    Activity done;
    TEST_ASSERT_TRUE(Parse("{\"v\":1,\"ts\":42,\"event\":\"post_tool\",\"state\":\"thinking\","
                           "\"tool_name\":\"Edit\",\"summary\":\"main.cpp\","
                           "\"agent_type\":\"Explore\"}",
                           &done));
    TEST_ASSERT_EQUAL_STRING("Edit", done.tool);
    TEST_ASSERT_EQUAL_STRING("main.cpp", done.summary);
    HeadlineOf(done, line, sizeof line);
    TEST_ASSERT_EQUAL_STRING("thinking", line);

    // The `running` half of the same pair still names it, agent and all — that is
    // the only state in which a tool name is the truth.
    HeadlineOf(Sample(), line, sizeof line);
    TEST_ASSERT_EQUAL_STRING("Explore > Bash - py -m pytest -q", line);
}

void test_the_headline_is_ascii_and_always_terminated(void) {
    char line[ui::kActivityHeadlineSize];
    HeadlineOf(Sample(), line, sizeof line);
    for (const char *at = line; *at != '\0'; ++at) {
        TEST_ASSERT_TRUE_MESSAGE(static_cast<unsigned char>(*at) < 0x80,
                                 "the panel's font is Montserrat's ASCII subset");
    }

    // A buffer far too small still comes back terminated and never overruns.
    char tiny[6] = {'z', 'z', 'z', 'z', 'z', 'z'};
    ActivityView view;
    view.Arrived(Sample(), 1000);
    view.Headline(tiny, sizeof tiny);
    TEST_ASSERT_EQUAL_CHAR('\0', tiny[sizeof tiny - 1]);
    TEST_ASSERT_TRUE(std::strlen(tiny) < sizeof tiny);
}

void test_nothing_arrived_is_not_a_line(void) {
    ActivityView view;
    TEST_ASSERT_FALSE(view.HasDocument());
    TEST_ASSERT_EQUAL_UINT32(0, view.Received());

    char line[ui::kActivityHeadlineSize];
    view.Headline(line, sizeof line);
    TEST_ASSERT_EQUAL_STRING("", line);
}

void test_the_view_counts_and_ages_what_arrives(void) {
    ActivityView view;
    view.Arrived(Sample(), 10000);
    TEST_ASSERT_TRUE(view.HasDocument());
    TEST_ASSERT_EQUAL_UINT32(1, view.Received());
    TEST_ASSERT_EQUAL_UINT32(0, view.AgeMs(10000));
    TEST_ASSERT_EQUAL_UINT32(2500, view.AgeMs(12500));

    view.Arrived(Sample(), 20000);
    TEST_ASSERT_EQUAL_UINT32(2, view.Received());
    TEST_ASSERT_EQUAL_UINT32(0, view.AgeMs(20000));
}

void test_a_running_document_goes_stale_and_an_idle_one_never_does(void) {
    ActivityView view;
    view.Arrived(Sample(), 1000);
    TEST_ASSERT_FALSE(view.Stale(1000));
    TEST_ASSERT_FALSE(view.Stale(1000 + ActivityView::kStaleMs - 1));
    TEST_ASSERT_TRUE(view.Stale(1000 + ActivityView::kStaleMs));

    // "idle" stays true until something else happens: fading it would suggest the
    // session vanished when it is simply done.
    Activity stop;
    TEST_ASSERT_TRUE(Parse(kStop, &stop));
    view.Arrived(stop, 1000);
    TEST_ASSERT_FALSE(view.Stale(1000 + 10 * ActivityView::kStaleMs));

    // And nothing at all is not stale either — there is nothing to disbelieve.
    ActivityView empty;
    TEST_ASSERT_FALSE(empty.Stale(999999));
}

void test_the_words_on_the_wire_have_one_spelling_each(void) {
    // The console prints these, and `commands.md` quotes them.
    TEST_ASSERT_EQUAL_STRING("pre_tool", ui::ActivityEventText(ActivityEvent::kPreTool));
    TEST_ASSERT_EQUAL_STRING("post_tool", ui::ActivityEventText(ActivityEvent::kPostTool));
    TEST_ASSERT_EQUAL_STRING("stop", ui::ActivityEventText(ActivityEvent::kStop));
    TEST_ASSERT_EQUAL_STRING("running", ui::ActivityStateText(ActivityState::kRunning));
    TEST_ASSERT_EQUAL_STRING("thinking", ui::ActivityStateText(ActivityState::kThinking));
    TEST_ASSERT_EQUAL_STRING("idle", ui::ActivityStateText(ActivityState::kIdle));
}

}  // namespace

void RegisterActivityTests(void) {
    RUN_TEST(test_a_real_activity_document_is_read);
    RUN_TEST(test_a_turn_that_ended_is_idle_and_says_nothing_about_a_tool);
    RUN_TEST(test_a_tool_that_has_run_is_thinking_not_idle);
    RUN_TEST(test_the_version_is_what_makes_a_document_ours);
    RUN_TEST(test_a_word_this_firmware_does_not_know_is_refused);
    RUN_TEST(test_junk_on_the_subject_keeps_the_last_good_document);
    RUN_TEST(test_a_document_too_long_to_be_ours_is_dropped_unread);
    RUN_TEST(test_text_longer_than_the_field_is_truncated_not_refused);
    RUN_TEST(test_truncation_never_splits_a_character);
    RUN_TEST(test_the_headline_reads_in_the_order_it_is_worth_reading);
    RUN_TEST(test_only_a_running_tool_is_named);
    RUN_TEST(test_the_headline_is_ascii_and_always_terminated);
    RUN_TEST(test_nothing_arrived_is_not_a_line);
    RUN_TEST(test_the_view_counts_and_ages_what_arrives);
    RUN_TEST(test_a_running_document_goes_stale_and_an_idle_one_never_does);
    RUN_TEST(test_the_words_on_the_wire_have_one_spelling_each);
}
