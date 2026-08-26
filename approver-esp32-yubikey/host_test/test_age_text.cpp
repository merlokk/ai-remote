// `ui::AgeText` (CLAUDE.md §10.11) — the duration formatter the console prints
// every age with.
//
// `cli/console.cpp` reaches for it in several places — how long the bus has been
// up, how long since the last state change, how long a request has left — rather
// than each readout formatting its own. So what this suite is worth is the bands
// being pinned: a paste of one readout and a paste of the next cannot describe the
// same instant two different ways.

#include <cstring>

#include "age_text.h"
#include "unity.h"

namespace {

// The age in the units a person reads, and the boundaries are the console's own:
// `PrintDuration` calls this function. `4000 s ago` is what this is for — the
// number a board left overnight actually showed before the bands existed.
void test_the_age_reads_the_way_the_console_prints_it(void) {
    char out[ui::kAgeTextSize];

    ui::AgeText(0, out, sizeof out);
    TEST_ASSERT_EQUAL_STRING("0 s", out);
    ui::AgeText(3, out, sizeof out);
    TEST_ASSERT_EQUAL_STRING("3 s", out);
    ui::AgeText(89, out, sizeof out);
    TEST_ASSERT_EQUAL_STRING("89 s", out);

    ui::AgeText(90, out, sizeof out);
    TEST_ASSERT_EQUAL_STRING("1 m", out);
    ui::AgeText(4000, out, sizeof out);
    TEST_ASSERT_EQUAL_STRING("66 m", out);
    ui::AgeText(5399, out, sizeof out);
    TEST_ASSERT_EQUAL_STRING("89 m", out);

    ui::AgeText(5400, out, sizeof out);
    TEST_ASSERT_EQUAL_STRING("1h 30m", out);
    ui::AgeText(8 * 3600 + 5 * 60, out, sizeof out);
    TEST_ASSERT_EQUAL_STRING("8h 05m", out);
    ui::AgeText(25 * 3600, out, sizeof out);
    TEST_ASSERT_EQUAL_STRING("25h 00m", out);
}

// Never writes past the end, and always terminated — the rule every string in
// this firmware is held to (§10.14.1).
void test_a_buffer_too_small_is_not_overrun(void) {
    char small[4];
    std::memset(small, 'x', sizeof small);
    ui::AgeText(5400, small, sizeof small);
    TEST_ASSERT_EQUAL_UINT32(3, std::strlen(small));
}

// The two ways a caller can hand this nothing at all. Neither is a crash, and
// neither writes anywhere.
void test_no_buffer_is_not_a_crash(void) {
    ui::AgeText(42, nullptr, 16);

    char untouched[2] = {'x', 'y'};
    ui::AgeText(42, untouched, 0);
    TEST_ASSERT_EQUAL_CHAR('x', untouched[0]);
    TEST_ASSERT_EQUAL_CHAR('y', untouched[1]);
}

// `kAgeTextSize` has to hold the longest thing the function can write, which is
// what a `uint32_t` of seconds reaches: `1193046h 28m`.
void test_the_declared_size_holds_the_longest_age(void) {
    char out[ui::kAgeTextSize];
    std::memset(out, 'x', sizeof out);
    ui::AgeText(0xFFFFFFFFu, out, sizeof out);
    TEST_ASSERT_EQUAL_STRING("1193046h 28m", out);
    TEST_ASSERT_TRUE(std::strlen(out) < ui::kAgeTextSize);
}

}  // namespace

void RegisterAgeTextTests(void) {
    RUN_TEST(test_the_age_reads_the_way_the_console_prints_it);
    RUN_TEST(test_a_buffer_too_small_is_not_overrun);
    RUN_TEST(test_no_buffer_is_not_a_crash);
    RUN_TEST(test_the_declared_size_holds_the_longest_age);
}
