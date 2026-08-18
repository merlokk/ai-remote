// The idle timer behind the panel (CLAUDE.md §10.8.1), tested where it costs
// nothing to test (§10.11, host tier).
//
// `idle_policy.h` includes `<cstdint>` and nothing else, so this suite needs no
// fake — the navigator's shape rather than the drivers'. It is the twelfth
// subject in this firmware to manage that.
//
// Two rules carry the value here, and they are the two that are invisible on a
// desk until months have passed:
//
//   * **the panel only goes off standing on its USB edge**, which is the one
//     orientation nobody reads a clock in, and the sign of that axis is what
//     §10.13 says is easy to get backwards and impossible to see wrong;
//   * **anything at all wakes it**, and the timer that decides is wrap-safe — a
//     device left alone for the ~49 days a millisecond counter takes to wrap is
//     exactly the device this feature is about.

#include <cstring>

#include "idle_policy.h"
#include "unity.h"

using ui::DisplayPower;
using ui::IdlePolicy;
using ui::IdleSettings;

namespace {

constexpr uint32_t kDimAfter = 900000;     // the shipped 15 minutes
constexpr uint32_t kSleepAfter = 1500000;  // and the shipped 25

IdleSettings Shipped() {
    IdleSettings settings;
    settings.dim_after_ms = kDimAfter;
    settings.sleep_after_ms = kSleepAfter;
    settings.full_percent = 80;
    settings.dim_percent = 30;
    return settings;
}

// A policy at full brightness with the clock started, which is where every one
// of these begins.
IdlePolicy Awake(uint32_t now_ms = 0) {
    IdlePolicy policy;
    policy.Configure(Shipped());
    policy.Activity(now_ms);
    policy.Tick(now_ms);
    return policy;
}

int State(const IdlePolicy &policy) { return static_cast<int>(policy.State()); }

constexpr int kFull = static_cast<int>(DisplayPower::kFull);
constexpr int kDim = static_cast<int>(DisplayPower::kDim);
constexpr int kOff = static_cast<int>(DisplayPower::kOff);

}  // namespace

// --- Dimming -------------------------------------------------------------

void test_it_starts_at_full_brightness(void) {
    IdlePolicy policy = Awake();
    TEST_ASSERT_EQUAL_INT(kFull, State(policy));
    TEST_ASSERT_EQUAL_UINT8(80, policy.Brightness());
}

void test_it_dims_after_the_configured_wait(void) {
    IdlePolicy policy = Awake();

    // One millisecond before, it is still the screen the operator left.
    TEST_ASSERT_FALSE(policy.Tick(kDimAfter - 1));
    TEST_ASSERT_EQUAL_INT(kFull, State(policy));

    TEST_ASSERT_TRUE(policy.Tick(kDimAfter));
    TEST_ASSERT_EQUAL_INT(kDim, State(policy));
    TEST_ASSERT_EQUAL_UINT8(30, policy.Brightness());
}

void test_the_change_is_reported_once(void) {
    // The caller sends a panel command on every `true`, and the panel is on the
    // same QSPI wires as the frame the operator is looking at.
    IdlePolicy policy = Awake();
    TEST_ASSERT_TRUE(policy.Tick(kDimAfter));
    TEST_ASSERT_FALSE(policy.Tick(kDimAfter + 1));
    TEST_ASSERT_FALSE(policy.Tick(kDimAfter + 60000));
}

void test_activity_brings_it_straight_back(void) {
    IdlePolicy policy = Awake();
    policy.Tick(kDimAfter);

    policy.Activity(kDimAfter + 5);
    TEST_ASSERT_TRUE(policy.Tick(kDimAfter + 5));
    TEST_ASSERT_EQUAL_INT(kFull, State(policy));
    TEST_ASSERT_EQUAL_UINT8(80, policy.Brightness());
}

void test_activity_restarts_the_wait(void) {
    IdlePolicy policy = Awake();
    policy.Activity(kDimAfter - 1);

    // The old deadline passes and nothing happens: the wait is measured from the
    // last thing that happened, not from the last time the screen was lit.
    TEST_ASSERT_FALSE(policy.Tick(kDimAfter));
    TEST_ASSERT_FALSE(policy.Tick(kDimAfter * 2 - 2));
    TEST_ASSERT_TRUE(policy.Tick(kDimAfter * 2 - 1));
}

void test_a_dim_of_zero_never_dims(void) {
    IdleSettings settings = Shipped();
    settings.dim_after_ms = 0;
    settings.sleep_after_ms = 0;

    IdlePolicy policy;
    policy.Configure(settings);
    policy.Activity(0);
    TEST_ASSERT_FALSE(policy.Tick(kDimAfter * 100));
    TEST_ASSERT_EQUAL_INT(kFull, State(policy));
}

void test_the_dim_level_never_exceeds_the_full_one(void) {
    // A file edited by hand can say brightness 20 and dimPercent 60, and a
    // screen that got *brighter* when it gave up waiting is a setting nobody
    // could explain.
    IdleSettings settings = Shipped();
    settings.full_percent = 20;
    settings.dim_percent = 60;

    IdlePolicy policy;
    policy.Configure(settings);
    policy.Activity(0);
    policy.Tick(kDimAfter);
    TEST_ASSERT_EQUAL_UINT8(20, policy.Brightness());
}

// --- The blank, and the orientation it needs -----------------------------

void test_it_never_goes_off_lying_flat(void) {
    // The whole of the second setting: a device on the desk is a clock, and a
    // clock that blanks itself is a black square somebody has to touch to read.
    IdlePolicy policy = Awake();
    policy.SetUpright(false);

    TEST_ASSERT_TRUE(policy.Tick(kDimAfter));
    TEST_ASSERT_FALSE(policy.Tick(kSleepAfter));
    TEST_ASSERT_FALSE(policy.Tick(kSleepAfter * 10));
    TEST_ASSERT_EQUAL_INT(kDim, State(policy));
}

void test_it_goes_off_standing_on_its_usb_edge(void) {
    IdlePolicy policy = Awake();
    policy.SetUpright(true);

    TEST_ASSERT_TRUE(policy.Tick(kDimAfter));
    TEST_ASSERT_FALSE(policy.Tick(kSleepAfter - 1));
    TEST_ASSERT_TRUE(policy.Tick(kSleepAfter));
    TEST_ASSERT_EQUAL_INT(kOff, State(policy));
}

void test_laying_it_down_asleep_brings_the_screen_back(void) {
    // Turning the board over is movement, so in practice this arrives with an
    // activity beside it — but the state is a function of what is true now, and
    // a device no longer in the position the rule names is not allowed to be
    // dark.
    IdlePolicy policy = Awake();
    policy.SetUpright(true);
    policy.Tick(kSleepAfter);

    policy.SetUpright(false);
    TEST_ASSERT_TRUE(policy.Tick(kSleepAfter + 1));
    TEST_ASSERT_EQUAL_INT(kDim, State(policy));
}

void test_activity_wakes_it_from_off(void) {
    IdlePolicy policy = Awake();
    policy.SetUpright(true);
    policy.Tick(kSleepAfter);

    policy.Activity(kSleepAfter + 10);
    TEST_ASSERT_TRUE(policy.Tick(kSleepAfter + 10));
    TEST_ASSERT_EQUAL_INT(kFull, State(policy));
    TEST_ASSERT_EQUAL_UINT8(80, policy.Brightness());
}

void test_a_sleep_of_zero_only_dims(void) {
    IdleSettings settings = Shipped();
    settings.sleep_after_ms = 0;

    IdlePolicy policy;
    policy.Configure(settings);
    policy.Activity(0);
    policy.SetUpright(true);
    policy.Tick(kDimAfter);
    TEST_ASSERT_FALSE(policy.Tick(kDimAfter * 1000));
    TEST_ASSERT_EQUAL_INT(kDim, State(policy));
}

void test_a_sleep_shorter_than_the_dim_still_goes_off(void) {
    // Nothing stops a hand-edited file saying sleepAfterSeconds 60 with
    // dimAfterSeconds 900. The panel going off is the stronger statement of the
    // two, so it is the one that wins.
    IdleSettings settings = Shipped();
    settings.dim_after_ms = 900000;
    settings.sleep_after_ms = 60000;

    IdlePolicy policy;
    policy.Configure(settings);
    policy.Activity(0);
    policy.SetUpright(true);
    TEST_ASSERT_TRUE(policy.Tick(60000));
    TEST_ASSERT_EQUAL_INT(kOff, State(policy));
}

// --- The wrap ------------------------------------------------------------

void test_the_wait_survives_the_millisecond_wrap(void) {
    // ~49 days, and the only way to reach it is to leave the thing alone — which
    // is exactly what this feature is about.
    const uint32_t before = 0xFFFFFFFFu - (kDimAfter / 2);

    IdlePolicy policy;
    policy.Configure(Shipped());
    policy.Activity(before);
    policy.Tick(before);

    TEST_ASSERT_FALSE(policy.Tick(before + kDimAfter - 1));  // wraps
    TEST_ASSERT_TRUE(policy.Tick(before + kDimAfter));
}

void test_the_blank_survives_the_millisecond_wrap(void) {
    const uint32_t before = 0xFFFFFFFFu - (kSleepAfter / 2);

    IdlePolicy policy;
    policy.Configure(Shipped());
    policy.Activity(before);
    policy.SetUpright(true);
    policy.Tick(before);

    TEST_ASSERT_TRUE(policy.Tick(before + kDimAfter));
    TEST_ASSERT_FALSE(policy.Tick(before + kSleepAfter - 1));
    TEST_ASSERT_TRUE(policy.Tick(before + kSleepAfter));
}

void test_the_idle_time_is_what_the_console_prints(void) {
    IdlePolicy policy = Awake(1000);
    TEST_ASSERT_EQUAL_UINT32(0, policy.IdleMs(1000));
    TEST_ASSERT_EQUAL_UINT32(5000, policy.IdleMs(6000));
}

// --- Settings changing under it ------------------------------------------

void test_a_shorter_wait_typed_in_applies_without_an_activity(void) {
    // `config set dim 60` on a screen that has been idle ten minutes should dim,
    // not wait another ten. Configure replaces the settings and nothing else —
    // the clock keeps running.
    IdlePolicy policy = Awake();
    policy.Tick(600000);
    TEST_ASSERT_EQUAL_INT(kFull, State(policy));

    IdleSettings settings = Shipped();
    settings.dim_after_ms = 60000;
    policy.Configure(settings);

    TEST_ASSERT_TRUE(policy.Tick(600001));
    TEST_ASSERT_EQUAL_INT(kDim, State(policy));
}

void test_configuring_does_not_count_as_activity(void) {
    // It is typed on the console, which is not the glass — and a device that
    // woke because somebody read its settings over USB is a device that never
    // sleeps while anybody is working near it.
    IdlePolicy policy = Awake();
    policy.Tick(kDimAfter);

    policy.Configure(Shipped());
    TEST_ASSERT_FALSE(policy.Tick(kDimAfter + 1));
    TEST_ASSERT_EQUAL_INT(kDim, State(policy));
}

// --- The two readings, which are the signs §10.13 says to measure ---------

void test_standing_on_the_usb_edge_is_buttons_up(void) {
    // The reading §10.13 measured for that position, not one derived from a
    // drawing: gravity along -Y, so the accelerometer reads +1 g on Y.
    TEST_ASSERT_TRUE(ui::StandingButtonsUp(0.0f, 1.0f, 0.0f));
}

void test_every_other_position_is_not(void) {
    // §10.13's table, every row of it.
    TEST_ASSERT_FALSE(ui::StandingButtonsUp(0.0f, 0.0f, -1.0f));       // flat, screen up
    TEST_ASSERT_FALSE(ui::StandingButtonsUp(0.0f, 0.0f, 1.0f));        // flat, screen down
    TEST_ASSERT_FALSE(ui::StandingButtonsUp(0.0f, -1.0f, 0.0f));       // on the button edge
    TEST_ASSERT_FALSE(ui::StandingButtonsUp(-1.012f, 0.067f, 0.007f)); // the card-slot edge
    TEST_ASSERT_FALSE(ui::StandingButtonsUp(1.0f, 0.0f, 0.0f));        // the speaker edge
}

void test_a_board_being_carried_is_no_position_at_all(void) {
    // Nothing dominant: on a corner, or in a hand. The blank needs a statement,
    // not the absence of one.
    TEST_ASSERT_FALSE(ui::StandingButtonsUp(0.5f, 0.5f, 0.5f));
    TEST_ASSERT_FALSE(ui::StandingButtonsUp(0.0f, 0.0f, 0.0f));
}

// --- The six positions, which are one table with two readers ---------------

void test_every_measured_position_is_named(void) {
    // §10.13's table, and it is the table itself: each row was established by
    // putting the board in that position and reading it, never derived from a
    // drawing. The numbers here are that section's, with the card-slot row given
    // as what this board actually reports.
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ui::Orientation::kScreenUp),
                          static_cast<int>(ui::OrientationOf(0.0f, 0.0f, -1.0f)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ui::Orientation::kScreenDown),
                          static_cast<int>(ui::OrientationOf(0.0f, 0.0f, 1.0f)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ui::Orientation::kUsbEdge),
                          static_cast<int>(ui::OrientationOf(0.0f, 1.0f, 0.0f)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ui::Orientation::kButtonEdge),
                          static_cast<int>(ui::OrientationOf(0.0f, -1.0f, 0.0f)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ui::Orientation::kSpeakerEdge),
                          static_cast<int>(ui::OrientationOf(1.0f, 0.0f, 0.0f)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ui::Orientation::kCardSlotEdge),
                          static_cast<int>(ui::OrientationOf(-1.012f, 0.067f, 0.007f)));
}

void test_no_position_is_its_own_answer(void) {
    // On a corner, in a hand, or being carried. §10.9's rule that `unknown` is
    // the honest state, arriving on a different subject.
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ui::Orientation::kUnknown),
                          static_cast<int>(ui::OrientationOf(0.5f, 0.5f, 0.5f)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ui::Orientation::kUnknown),
                          static_cast<int>(ui::OrientationOf(0.0f, 0.0f, 0.0f)));
}

void test_standing_up_is_the_same_table(void) {
    // **One source of truth, and this is what says so.** The blank of §10.8.1
    // and the line on the status page must never disagree about which way up the
    // board is — a screen saying it is standing while the panel refuses to go
    // dark is a bug nobody could diagnose from the outside.
    const float samples[][3] = {
        {0.0f, 1.0f, 0.0f},   {0.0f, -1.0f, 0.0f},  {0.0f, 0.0f, 1.0f},
        {0.0f, 0.0f, -1.0f},  {1.0f, 0.0f, 0.0f},   {-1.0f, 0.0f, 0.0f},
        {0.5f, 0.5f, 0.5f},   {0.1f, 0.85f, -0.4f}, {0.0f, 0.0f, 0.0f},
    };
    for (const auto &s : samples) {
        const bool named = ui::OrientationOf(s[0], s[1], s[2]) == ui::Orientation::kUsbEdge;
        TEST_ASSERT_EQUAL_INT(named, ui::StandingButtonsUp(s[0], s[1], s[2]));
    }
}

void test_every_name_fits_the_status_page(void) {
    // **Nineteen, and the number was measured on the glass rather than counted.**
    // `screens/status_screen.h` sizes the value buffer at 22 and used to claim
    // that made 21 characters safe; the first name written to that budget — `on
    // the card-slot edge`, exactly 21 — was photographed with §10.12.2 and its
    // rightmost lit pixel was column **479 of 479**, which is a word cut off by
    // the edge of the panel. A character budget is only true for the characters
    // it was counted with, and these are wide ones.
    //
    // So the names are shorter than the buffer, and this is where that is
    // enforced: `ui` cannot see the layout, and the layout cannot see the names.
    constexpr size_t kMeasuredBudget = 19;
    for (int i = 0; i <= static_cast<int>(ui::Orientation::kCardSlotEdge); ++i) {
        const char *name = ui::OrientationName(static_cast<ui::Orientation>(i));
        TEST_ASSERT_NOT_NULL(name);
        TEST_ASSERT_TRUE_MESSAGE(strlen(name) <= kMeasuredBudget, name);
        // And the axis, which is what the console prints in front of it.
        TEST_ASSERT_NOT_NULL(ui::OrientationAxis(static_cast<ui::Orientation>(i)));
    }
}

void test_the_unknown_position_has_no_axis(void) {
    // The console prints `gravity along <axis> (<name>)`, and there is no axis
    // to name when nothing dominates — an empty string is what lets that one
    // caller take the other branch rather than printing `along  (…)`.
    TEST_ASSERT_EQUAL_STRING("", ui::OrientationAxis(ui::Orientation::kUnknown));
    TEST_ASSERT_TRUE(ui::OrientationAxis(ui::Orientation::kUsbEdge)[0] != 0);
}

void test_a_tilted_stand_still_counts(void) {
    // A stand holds the board leaning back; 0.85 g on Y with the rest spread is
    // still somebody's desk ornament standing up.
    TEST_ASSERT_TRUE(ui::StandingButtonsUp(0.1f, 0.85f, -0.4f));
}

void test_the_noise_of_a_board_at_rest_is_not_movement(void) {
    // Measured on this board: the magnitude wanders about 0.02 g at rest and the
    // gyro sits at -3 dps. A threshold under that is a device that never sleeps.
    const float first[3] = {-1.012f, 0.067f, 0.007f};
    const float second[3] = {-1.008f, 0.072f, 0.001f};
    TEST_ASSERT_FALSE(ui::Moved(first, second));
}

void test_picking_it_up_is_movement(void) {
    const float on_the_desk[3] = {0.0f, 0.0f, -1.0f};
    const float in_a_hand[3] = {0.12f, 0.30f, -0.94f};
    TEST_ASSERT_TRUE(ui::Moved(on_the_desk, in_a_hand));
}

void test_movement_is_the_whole_vector_and_not_one_axis(void) {
    // Three small changes that add up to a real one: a rotation about the
    // gravity vector moves every axis a little and none of them much.
    const float before[3] = {0.58f, 0.58f, 0.58f};
    const float after[3] = {0.52f, 0.64f, 0.52f};
    TEST_ASSERT_TRUE(ui::Moved(before, after));
}

void RegisterIdleTests(void) {
    RUN_TEST(test_it_starts_at_full_brightness);
    RUN_TEST(test_it_dims_after_the_configured_wait);
    RUN_TEST(test_the_change_is_reported_once);
    RUN_TEST(test_activity_brings_it_straight_back);
    RUN_TEST(test_activity_restarts_the_wait);
    RUN_TEST(test_a_dim_of_zero_never_dims);
    RUN_TEST(test_the_dim_level_never_exceeds_the_full_one);

    RUN_TEST(test_it_never_goes_off_lying_flat);
    RUN_TEST(test_it_goes_off_standing_on_its_usb_edge);
    RUN_TEST(test_laying_it_down_asleep_brings_the_screen_back);
    RUN_TEST(test_activity_wakes_it_from_off);
    RUN_TEST(test_a_sleep_of_zero_only_dims);
    RUN_TEST(test_a_sleep_shorter_than_the_dim_still_goes_off);

    RUN_TEST(test_the_wait_survives_the_millisecond_wrap);
    RUN_TEST(test_the_blank_survives_the_millisecond_wrap);
    RUN_TEST(test_the_idle_time_is_what_the_console_prints);

    RUN_TEST(test_a_shorter_wait_typed_in_applies_without_an_activity);
    RUN_TEST(test_configuring_does_not_count_as_activity);

    RUN_TEST(test_standing_on_the_usb_edge_is_buttons_up);
    RUN_TEST(test_every_other_position_is_not);
    RUN_TEST(test_a_board_being_carried_is_no_position_at_all);
    RUN_TEST(test_a_tilted_stand_still_counts);

    RUN_TEST(test_every_measured_position_is_named);
    RUN_TEST(test_no_position_is_its_own_answer);
    RUN_TEST(test_standing_up_is_the_same_table);
    RUN_TEST(test_every_name_fits_the_status_page);
    RUN_TEST(test_the_unknown_position_has_no_axis);

    RUN_TEST(test_the_noise_of_a_board_at_rest_is_not_movement);
    RUN_TEST(test_picking_it_up_is_movement);
    RUN_TEST(test_movement_is_the_whole_vector_and_not_one_axis);
}
