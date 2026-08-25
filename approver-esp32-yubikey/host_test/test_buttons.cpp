// The button (CLAUDE.md §10.1, §10.15).
//
// There is no chip here, so what is under test is the part that is easy to get
// subtly wrong: **a stable answer to "is it pressed", and how long it has been
// that way.** `Debounce` is deliberately a class of its own with no ESP-IDF in
// it — its header says so — and that is what most of this file exercises.
//
// The two the rest of the firmware actually leans on:
//
//   * **`active_low` per button**. This board wires its one button the ordinary
//     way, but the sibling board found `PWR` wired the other way round, and a
//     driver that assumed one polarity reported a button pressed for the whole
//     uptime. The field stays, and so does the test;
//   * **`HeldFor`**, which is §10.15's config restore.
//
// **The table below is still three buttons, and that is deliberate.** This board
// has one; the class is a table of pins either way, and testing it with three
// exercises the "one press does not move the others" case that a single-button
// table cannot express. What the board actually wires is `components/boards`'s
// business, and `test_led`/`test_indicator` are where this board's own decisions
// are pinned.

#include "buttons.h"
#include "fake_platform.h"
#include "unity.h"

namespace {

constexpr gpio_num_t kBootPin = GPIO_NUM_9;
constexpr gpio_num_t kKeyPin = GPIO_NUM_10;
constexpr gpio_num_t kPwrPin = GPIO_NUM_18;

// The board's real table (`components/boards/board.cpp`), including the
// polarity that was measured rather than assumed.
constexpr buttons::Config kBoard[] = {
    {.pin = kBootPin, .name = "boot", .active_low = true, .pull_up = true},
    {.pin = kKeyPin, .name = "key", .active_low = true, .pull_up = true},
    {.pin = kPwrPin, .name = "pwr", .active_low = false, .pull_up = false},
};

enum : size_t { kBoot = 0, kKey = 1, kPwr = 2 };

// A contact at rest. `active_low` buttons idle high; `PWR` idles low.
void ReleaseAll() {
    fake::SetPinLevel(kBootPin, 1);
    fake::SetPinLevel(kKeyPin, 1);
    fake::SetPinLevel(kPwrPin, 0);
}

void Press(gpio_num_t pin, bool active_low) { fake::SetPinLevel(pin, active_low ? 0 : 1); }
void Release(gpio_num_t pin, bool active_low) { fake::SetPinLevel(pin, active_low ? 1 : 0); }

// By reference: `Buttons` owns its pins and deletes its copy operations, which
// is the same property §10.14.1 asks of anything holding a resource — so a test
// declares one and hands it here rather than getting one back.
void BringUp(buttons::Buttons &keys) {
    ReleaseAll();
    TEST_ASSERT_EQUAL_INT(ESP_OK, keys.Init(kBoard, 3));
    // `gpio_config` sets the idle level from the pull, so re-assert what the
    // test wants on top of it.
    ReleaseAll();
    keys.PollAll();
}

}  // namespace

// --- The debounce, on its own --------------------------------------------

void test_debounce_adopts_a_level_without_reporting_an_edge(void) {
    // What `Init` does: a device booted with a finger already down must not see
    // a press it was not there for.
    buttons::Debounce d;
    d.Reset(true, 1000);
    TEST_ASSERT_TRUE(d.Pressed());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(buttons::Event::kNone),
                          static_cast<int>(d.Update(true, 1000 + buttons::kDebounceMs * 2)));
}

void test_debounce_waits_out_the_window_before_believing_a_press(void) {
    buttons::Debounce d;
    d.Reset(false, 0);

    // **The window starts when the new level is first seen**, not when the
    // last one settled — so the clock for it runs from this call, not from the
    // Reset above. Getting that wrong in a test is harmless; getting it wrong
    // in the driver would make the window shorter than it claims.
    constexpr uint32_t kFirstSeen = 1;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(buttons::Event::kNone),
                          static_cast<int>(d.Update(true, kFirstSeen)));
    TEST_ASSERT_FALSE(d.Pressed());
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(buttons::Event::kNone),
        static_cast<int>(d.Update(true, kFirstSeen + buttons::kDebounceMs - 1)));
    TEST_ASSERT_FALSE(d.Pressed());

    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(buttons::Event::kPressed),
        static_cast<int>(d.Update(true, kFirstSeen + buttons::kDebounceMs)));
    TEST_ASSERT_TRUE(d.Pressed());
}

void test_debounce_reports_a_press_once(void) {
    buttons::Debounce d;
    d.Reset(false, 0);
    d.Update(true, 0);
    d.Update(true, buttons::kDebounceMs);

    // Still held is not a new press. A caller that acted on every kPressed
    // would restore the config once per poll.
    TEST_ASSERT_EQUAL_INT(static_cast<int>(buttons::Event::kNone),
                          static_cast<int>(d.Update(true, buttons::kDebounceMs + 100)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(buttons::Event::kNone),
                          static_cast<int>(d.Update(true, buttons::kDebounceMs + 200)));
}

void test_debounce_swallows_a_bounce_shorter_than_the_window(void) {
    // **The whole reason this class exists.** A contact that chatters for a few
    // milliseconds is one press, not four — §10.7 says a `buttons watch` run
    // showing several 30 ms presses where one finger went down is a window that
    // is too short, and this is that rule from the other side.
    buttons::Debounce d;
    d.Reset(false, 0);

    for (uint32_t t = 1; t < buttons::kDebounceMs; t += 2) {
        TEST_ASSERT_EQUAL_INT(static_cast<int>(buttons::Event::kNone),
                              static_cast<int>(d.Update(t % 4 == 1, t)));
    }
    TEST_ASSERT_FALSE(d.Pressed());
}

void test_debounce_a_bounce_that_settles_back_reports_nothing(void) {
    buttons::Debounce d;
    d.Reset(false, 0);

    d.Update(true, 1);   // a spike…
    d.Update(false, 5);  // …gone before the window
    TEST_ASSERT_EQUAL_INT(static_cast<int>(buttons::Event::kNone),
                          static_cast<int>(d.Update(false, 5 + buttons::kDebounceMs * 2)));
    TEST_ASSERT_FALSE(d.Pressed());
}

void test_debounce_reports_the_release_edge_too(void) {
    buttons::Debounce d;
    d.Reset(false, 0);
    d.Update(true, 0);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(buttons::Event::kPressed),
                          static_cast<int>(d.Update(true, buttons::kDebounceMs)));

    const uint32_t up = buttons::kDebounceMs + 500;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(buttons::Event::kNone),
                          static_cast<int>(d.Update(false, up)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(buttons::Event::kReleased),
                          static_cast<int>(d.Update(false, up + buttons::kDebounceMs)));
    TEST_ASSERT_FALSE(d.Pressed());
}

void test_debounce_measures_how_long_the_current_state_has_held(void) {
    // Pressed *or* released — the header is explicit, and §10.7's readout
    // depends on it: a pin held low by a fault reads pressed for the whole
    // uptime, which is how a broken button tells itself apart from an idle one.
    buttons::Debounce d;
    d.Reset(false, 1000);
    TEST_ASSERT_EQUAL_UINT32(500, d.StableMs(1500));

    d.Update(true, 2000);
    d.Update(true, 2000 + buttons::kDebounceMs);  // the edge lands here
    TEST_ASSERT_EQUAL_UINT32(100, d.StableMs(2000 + buttons::kDebounceMs + 100));
}

void test_debounce_survives_the_millisecond_counter_wrapping(void) {
    // The header says times are only ever subtracted, so the wrap at ~49 days
    // is not a special case. This is that claim, tested — a device on a desk
    // for two months is not a hypothetical.
    constexpr uint32_t kNearWrap = 0xFFFFFFF0;
    buttons::Debounce d;
    d.Reset(false, kNearWrap);

    // 16 ms later the counter is at 0; 25 ms later it is at 9.
    TEST_ASSERT_EQUAL_UINT32(16, d.StableMs(0));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(buttons::Event::kNone),
                          static_cast<int>(d.Update(true, kNearWrap + 1)));
    // The counter has wrapped past zero by the time the window is up, which is
    // the case the subtraction has to survive.
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(buttons::Event::kPressed),
        static_cast<int>(d.Update(true, kNearWrap + 1 + buttons::kDebounceMs)));
    TEST_ASSERT_TRUE(d.Pressed());
}

// --- The pins ------------------------------------------------------------

void test_buttons_init_takes_the_board_table(void) {
    buttons::Buttons keys;
    BringUp(keys);
    TEST_ASSERT_TRUE(keys.Ready());
    TEST_ASSERT_EQUAL_UINT(3, keys.Count());
    TEST_ASSERT_EQUAL_STRING("key", keys.Name(kKey));
    TEST_ASSERT_EQUAL_INT(kKeyPin, keys.Gpio(kKey));
}

void test_buttons_refuses_more_than_it_has_slots_for(void) {
    // §10.14.1: full is a designed state, not an error discovered later.
    buttons::Config many[buttons::kMaxButtons + 2] = {};
    for (auto &config : many) {
        config.pin = GPIO_NUM_9;
        config.name = "x";
    }

    buttons::Buttons keys;
    TEST_ASSERT_NOT_EQUAL(ESP_OK, keys.Init(many, buttons::kMaxButtons + 2));
}

void test_buttons_refuses_a_table_it_cannot_use(void) {
    // No table, no buttons, and a button with no pin. The last one is the
    // realistic mistake: `board.h` is a hand-written map (§10.1), and a row
    // that has not been filled in yet reads as `GPIO_NUM_NC` — which
    // `gpio_config` would happily turn into a mask of `1 << -1`.
    buttons::Buttons keys;
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_ARG, keys.Init(nullptr, 3));
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_ARG, keys.Init(kBoard, 0));
    TEST_ASSERT_FALSE(keys.Ready());

    const buttons::Config unwired[] = {
        {.pin = kBootPin, .name = "boot", .active_low = true, .pull_up = true},
        {.pin = GPIO_NUM_NC, .name = "key", .active_low = true, .pull_up = true},
    };
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_ARG, keys.Init(unwired, 2));
    TEST_ASSERT_FALSE(keys.Ready());
    TEST_ASSERT_EQUAL_UINT(0, keys.Count());
}

void test_buttons_active_low_and_active_high_are_both_honoured(void) {
    // **Both polarities, on a board that only has one of them.** `BOOT` shorts
    // its pin to ground; the third entry in this table rests at 0 and goes high
    // while held. This device has no such button — the sibling board's `PWR` was
    // the inverted one — and the case is kept because the table is shared and
    // assuming a single polarity once gave a button that read as pressed for the
    // whole uptime.
    buttons::Buttons keys;
    BringUp(keys);

    TEST_ASSERT_FALSE(keys.RawPressed(kKey));
    TEST_ASSERT_FALSE(keys.RawPressed(kPwr));

    Press(kKeyPin, true);
    Press(kPwrPin, false);
    TEST_ASSERT_TRUE(keys.RawPressed(kKey));
    TEST_ASSERT_TRUE(keys.RawPressed(kPwr));

    // And the same pin level means opposite things for the two of them.
    fake::SetPinLevel(kKeyPin, 1);
    fake::SetPinLevel(kPwrPin, 1);
    TEST_ASSERT_FALSE(keys.RawPressed(kKey));
    TEST_ASSERT_TRUE(keys.RawPressed(kPwr));
}

void test_buttons_poll_reports_a_debounced_edge(void) {
    buttons::Buttons keys;
    BringUp(keys);

    Press(kKeyPin, true);
    // The first poll sees the new level and starts the window; the edge comes
    // one full window after that.
    TEST_ASSERT_EQUAL_INT(static_cast<int>(buttons::Event::kNone),
                          static_cast<int>(keys.Poll(kKey)));

    fake::AdvanceMs(buttons::kDebounceMs);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(buttons::Event::kPressed),
                          static_cast<int>(keys.Poll(kKey)));
    TEST_ASSERT_TRUE(keys.Pressed(kKey));

    Release(kKeyPin, true);
    keys.Poll(kKey);  // sees the new level, starts the window
    fake::AdvanceMs(buttons::kDebounceMs);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(buttons::Event::kReleased),
                          static_cast<int>(keys.Poll(kKey)));
    TEST_ASSERT_FALSE(keys.Pressed(kKey));
}

void test_buttons_one_press_does_not_move_the_others(void) {
    buttons::Buttons keys;
    BringUp(keys);

    Press(kKeyPin, true);
    keys.PollAll();
    fake::AdvanceMs(buttons::kDebounceMs);
    keys.PollAll();

    TEST_ASSERT_TRUE(keys.Pressed(kKey));
    TEST_ASSERT_FALSE(keys.Pressed(kBoot));
    TEST_ASSERT_FALSE(keys.Pressed(kPwr));
}

void test_buttons_an_index_that_is_not_there_answers_safely(void) {
    // The console takes an index from an operator, so out of range has to be a
    // quiet false rather than a read past the array.
    buttons::Buttons keys;
    BringUp(keys);
    TEST_ASSERT_FALSE(keys.Pressed(99));
    TEST_ASSERT_FALSE(keys.RawPressed(99));
    TEST_ASSERT_EQUAL_UINT32(0, keys.StableMs(99));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(buttons::Event::kNone),
                          static_cast<int>(keys.Poll(99)));
}

// --- HeldFor, which is §10.15's config restore ---------------------------

void test_buttons_init_adopts_a_button_that_is_already_down(void) {
    // **§10.15's actual scenario**, and the reason `Init` reads the pin instead
    // of assuming released: the finger is on `KEY` before the board powers on,
    // so there is no edge to wait for and no window to sit out. A driver that
    // started from "released" would need 25 ms of held button before it agreed
    // anything was pressed, and the restore check runs before that.
    buttons::Buttons keys;
    fake::SetPinLevel(kKeyPin, 0);  // held, active low
    TEST_ASSERT_EQUAL_INT(ESP_OK, keys.Init(kBoard, 3));

    TEST_ASSERT_TRUE(keys.Pressed(kKey));
    TEST_ASSERT_FALSE(keys.Pressed(kBoot));
}

void test_buttons_held_for_returns_true_after_the_hold(void) {
    // Held from before boot, which is how §10.15's restore is reached — `Init`
    // adopts the level, so the first poll already agrees it is down.
    buttons::Buttons keys;
    fake::SetPinLevel(kKeyPin, 0);
    TEST_ASSERT_EQUAL_INT(ESP_OK, keys.Init(kBoard, 3));

    // The fake clock advances inside `vTaskDelay`, so this terminates on its
    // own — the loop really does poll its way to five seconds.
    TEST_ASSERT_TRUE(keys.HeldFor(kKey, 5000));
    TEST_ASSERT_TRUE(fake::P().delay_ms_total >= 5000);
}

void test_buttons_held_for_gives_up_the_moment_it_is_released(void) {
    // §10.15: released early, nothing happens. There is no feedback that early
    // in boot to make a partial press meaningful, which is the argument for a
    // long threshold — and for this returning false rather than "nearly".
    buttons::Buttons keys;
    fake::SetPinLevel(kKeyPin, 0);
    TEST_ASSERT_EQUAL_INT(ESP_OK, keys.Init(kBoard, 3));

    // Let it get part of the way, then let go. The release has to survive its
    // own debounce window before `HeldFor` believes it, which is why the poll
    // loop inside is what notices rather than the level alone.
    TEST_ASSERT_TRUE(keys.HeldFor(kKey, 1000));
    Release(kKeyPin, true);
    TEST_ASSERT_FALSE(keys.HeldFor(kKey, 5000));
}

void test_buttons_held_for_is_false_when_nothing_was_pressed(void) {
    // Immediately, and this is the common case: every boot where nobody is
    // holding anything must not cost five seconds.
    buttons::Buttons keys;
    BringUp(keys);

    const uint32_t before = fake::P().delay_ms_total;
    TEST_ASSERT_FALSE(keys.HeldFor(kKey, 5000));
    TEST_ASSERT_TRUE(fake::P().delay_ms_total - before < 1000);
}

void test_buttons_held_for_on_a_bad_index_is_false(void) {
    buttons::Buttons keys;
    BringUp(keys);
    TEST_ASSERT_FALSE(keys.HeldFor(99, 5000));
}

// --- Short press or long press (§10.8.5) ---------------------------------

void test_a_press_released_early_is_a_short_one(void) {
    buttons::PressLength key(2000);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(buttons::Press::kNone),
                          static_cast<int>(key.Update(true, 1000)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(buttons::Press::kNone),
                          static_cast<int>(key.Update(true, 2500)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(buttons::Press::kShort),
                          static_cast<int>(key.Update(false, 2900)));
}

void test_a_long_press_fires_while_the_finger_is_still_down(void) {
    // The decision this class exists for: the operator is holding a button with
    // no feedback but the screen, so the screen has to change under their finger
    // rather than when they give up and let go.
    buttons::PressLength key(2000);
    key.Update(true, 1000);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(buttons::Press::kNone),
                          static_cast<int>(key.Update(true, 2999)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(buttons::Press::kLong),
                          static_cast<int>(key.Update(true, 3000)));
}

void test_a_long_press_fires_once_and_the_release_says_nothing(void) {
    // Otherwise one press would open settings and then immediately activate
    // whatever row it landed on.
    buttons::PressLength key(2000);
    key.Update(true, 1000);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(buttons::Press::kLong),
                          static_cast<int>(key.Update(true, 3000)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(buttons::Press::kNone),
                          static_cast<int>(key.Update(true, 4000)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(buttons::Press::kNone),
                          static_cast<int>(key.Update(false, 5000)));
}

void test_the_next_press_after_a_long_one_starts_over(void) {
    buttons::PressLength key(2000);
    key.Update(true, 1000);
    key.Update(true, 3000);
    key.Update(false, 3100);

    key.Update(true, 4000);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(buttons::Press::kShort),
                          static_cast<int>(key.Update(false, 4200)));
}

void test_a_button_nobody_is_touching_reports_nothing(void) {
    buttons::PressLength key(2000);
    for (uint32_t t = 0; t < 10000; t += 20) {
        TEST_ASSERT_EQUAL_INT(static_cast<int>(buttons::Press::kNone),
                              static_cast<int>(key.Update(false, t)));
    }
    TEST_ASSERT_EQUAL_UINT32(0, key.HeldMs(10000));
}

void test_the_hold_is_measured_across_the_millisecond_wrap(void) {
    buttons::PressLength key(2000);
    key.Update(true, 0xFFFFFF00u);
    TEST_ASSERT_EQUAL_UINT32(656, key.HeldMs(400));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(buttons::Press::kLong),
                          static_cast<int>(key.Update(true, 0xFFFFFF00u + 2000)));
}

void RegisterButtonsTests(void) {
    RUN_TEST(test_debounce_adopts_a_level_without_reporting_an_edge);
    RUN_TEST(test_debounce_waits_out_the_window_before_believing_a_press);
    RUN_TEST(test_debounce_reports_a_press_once);
    RUN_TEST(test_debounce_swallows_a_bounce_shorter_than_the_window);
    RUN_TEST(test_debounce_a_bounce_that_settles_back_reports_nothing);
    RUN_TEST(test_debounce_reports_the_release_edge_too);
    RUN_TEST(test_debounce_measures_how_long_the_current_state_has_held);
    RUN_TEST(test_debounce_survives_the_millisecond_counter_wrapping);

    RUN_TEST(test_buttons_init_takes_the_board_table);
    RUN_TEST(test_buttons_refuses_more_than_it_has_slots_for);
    RUN_TEST(test_buttons_refuses_a_table_it_cannot_use);
    RUN_TEST(test_buttons_active_low_and_active_high_are_both_honoured);
    RUN_TEST(test_buttons_poll_reports_a_debounced_edge);
    RUN_TEST(test_buttons_one_press_does_not_move_the_others);
    RUN_TEST(test_buttons_an_index_that_is_not_there_answers_safely);

    RUN_TEST(test_buttons_init_adopts_a_button_that_is_already_down);
    RUN_TEST(test_buttons_held_for_returns_true_after_the_hold);
    RUN_TEST(test_buttons_held_for_gives_up_the_moment_it_is_released);
    RUN_TEST(test_buttons_held_for_is_false_when_nothing_was_pressed);
    RUN_TEST(test_buttons_held_for_on_a_bad_index_is_false);

    RUN_TEST(test_a_press_released_early_is_a_short_one);
    RUN_TEST(test_a_long_press_fires_while_the_finger_is_still_down);
    RUN_TEST(test_a_long_press_fires_once_and_the_release_says_nothing);
    RUN_TEST(test_the_next_press_after_a_long_one_starts_over);
    RUN_TEST(test_a_button_nobody_is_touching_reports_nothing);
    RUN_TEST(test_the_hold_is_measured_across_the_millisecond_wrap);
}
