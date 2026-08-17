// The clock screen's decisions (CLAUDE.md §10.8.2), tested where they cost
// nothing to test (§10.11, host tier).
//
// `clock_face.h` includes `<cstdint>` and nothing else, so this suite needs no
// fake at all — the navigator's shape rather than the drivers'.
//
// Five blocks, and each is a rule from §10.8:
//
//   * the time, whose whole point is refusing to show a plausible wrong one;
//   * the three indicators, each of which has a state that is *not* a fault;
//   * the bus dot's two-minute window, which is the one piece of state in here;
//   * the drift, which exists because the panel is an AMOLED and must therefore
//     both stay in its box and never stop moving;
//   * the water, which must be bounded, periodic, and actually travel.

#include <cstring>

#include "clock_face.h"
#include "unity.h"

using ui::BatteryIcon;
using ui::BusIcon;
using ui::ClockFace;
using ui::ClockInputs;
using ui::ClockView;
using ui::WifiIcon;

namespace {

// 2026-08-17 00:00:00 UTC — a believable epoch, so a test about anything other
// than the clock does not have to think about the clock.
constexpr int64_t kNow = 1786924800;

ClockInputs Sane() {
    ClockInputs in;
    in.epoch_utc = kNow;
    in.hour = 13;
    in.minute = 45;
    return in;
}

// --- The time ------------------------------------------------------------

void test_an_unset_clock_shows_dashes_rather_than_midnight(void) {
    ClockFace face;
    ClockInputs in;  // epoch 0, hour 0, minute 0 — i.e. a plausible "00:00"
    const ClockView view = face.Update(in);

    TEST_ASSERT_FALSE(view.time_valid);
    for (int i = 0; i < 4; ++i) {
        TEST_ASSERT_EQUAL_INT8(ClockView::kDash, view.digit[i]);
    }
}

void test_a_year_before_the_rtc_can_hold_is_not_believed(void) {
    ClockFace face;
    ClockInputs in = Sane();
    in.epoch_utc = ClockFace::kEpochFloor - 1;
    TEST_ASSERT_FALSE(face.Update(in).time_valid);

    in.epoch_utc = ClockFace::kEpochFloor;
    TEST_ASSERT_TRUE(face.Update(in).time_valid);
}

void test_a_year_past_the_rtc_is_refused_the_way_the_rtc_refuses_it(void) {
    // §10.8.2: the PCF85063 stores two digits and no century, so 2100 is not a
    // time this device can hold. The face and the sync refuse the same answers.
    ClockFace face;
    ClockInputs in = Sane();
    in.epoch_utc = ClockFace::kEpochCeiling;
    TEST_ASSERT_FALSE(face.Update(in).time_valid);

    in.epoch_utc = ClockFace::kEpochCeiling - 1;
    TEST_ASSERT_TRUE(face.Update(in).time_valid);
}

void test_the_digits_are_twenty_four_hour(void) {
    ClockFace face;
    ClockInputs in = Sane();

    in.hour = 0;
    in.minute = 7;
    ClockView view = face.Update(in);
    TEST_ASSERT_TRUE(view.time_valid);
    TEST_ASSERT_EQUAL_INT8(0, view.digit[0]);
    TEST_ASSERT_EQUAL_INT8(0, view.digit[1]);
    TEST_ASSERT_EQUAL_INT8(0, view.digit[2]);
    TEST_ASSERT_EQUAL_INT8(7, view.digit[3]);

    // The hour a 12-hour clock would have called 11, and the leading zero that
    // a "%d" would have dropped.
    in.hour = 23;
    in.minute = 59;
    view = face.Update(in);
    TEST_ASSERT_EQUAL_INT8(2, view.digit[0]);
    TEST_ASSERT_EQUAL_INT8(3, view.digit[1]);
    TEST_ASSERT_EQUAL_INT8(5, view.digit[2]);
    TEST_ASSERT_EQUAL_INT8(9, view.digit[3]);

    in.hour = 9;
    in.minute = 5;
    view = face.Update(in);
    TEST_ASSERT_EQUAL_INT8(0, view.digit[0]);
    TEST_ASSERT_EQUAL_INT8(9, view.digit[1]);
    TEST_ASSERT_EQUAL_INT8(0, view.digit[2]);
    TEST_ASSERT_EQUAL_INT8(5, view.digit[3]);
}

void test_a_wall_clock_out_of_range_shows_dashes(void) {
    // A believable epoch and an impossible hour is not a combination `localtime`
    // produces — which is exactly why it must not be trusted to be impossible.
    ClockFace face;
    ClockInputs in = Sane();
    in.hour = 24;
    TEST_ASSERT_FALSE(face.Update(in).time_valid);

    in.hour = 13;
    in.minute = 60;
    TEST_ASSERT_FALSE(face.Update(in).time_valid);
}

// --- The Wi-Fi icon ------------------------------------------------------

void test_bars_climb_with_the_signal(void) {
    TEST_ASSERT_EQUAL_UINT8(3, ClockFace::BarsFor(-40));
    TEST_ASSERT_EQUAL_UINT8(2, ClockFace::BarsFor(-66));
    TEST_ASSERT_EQUAL_UINT8(1, ClockFace::BarsFor(-88));
}

void test_a_connected_link_never_shows_no_bars(void) {
    // A dreadful signal is still a connection, and an icon with nothing lit
    // reads as an icon saying "not connected" (§10.9: a weak signal is not a
    // failure state).
    for (int rssi = -110; rssi <= 0; ++rssi) {
        const uint8_t bars = ClockFace::BarsFor(static_cast<int8_t>(rssi));
        TEST_ASSERT_GREATER_OR_EQUAL_UINT8(1, bars);
        TEST_ASSERT_LESS_OR_EQUAL_UINT8(ClockFace::kMaxBars, bars);
    }
}

void test_a_client_link_reports_its_bars(void) {
    ClockFace face;
    ClockInputs in = Sane();
    in.wifi = WifiIcon::kClient;
    in.rssi = -50;

    const ClockView view = face.Update(in);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(WifiIcon::kClient), static_cast<int>(view.wifi));
    TEST_ASSERT_EQUAL_UINT8(3, view.bars);
}

void test_connecting_animates_and_is_never_blank(void) {
    // "connecting" with nothing lit is indistinguishable from "off", and those
    // are the two states an operator most needs told apart.
    ClockFace face;
    ClockInputs in = Sane();
    in.wifi = WifiIcon::kConnecting;

    bool seen[ClockFace::kMaxBars + 1] = {};
    for (uint32_t ms = 0; ms < 4000; ms += 100) {
        in.now_ms = ms;
        const ClockView view = face.Update(in);
        TEST_ASSERT_GREATER_OR_EQUAL_UINT8(1, view.bars);
        TEST_ASSERT_LESS_OR_EQUAL_UINT8(ClockFace::kMaxBars, view.bars);
        seen[view.bars] = true;
    }
    for (uint8_t bars = 1; bars <= ClockFace::kMaxBars; ++bars) {
        TEST_ASSERT_TRUE_MESSAGE(seen[bars], "the connecting animation stalled");
    }
}

void test_the_access_point_and_the_radio_off_light_no_bars(void) {
    // §10.9: this device *is* the network, so there is no signal to report —
    // and the icon says so with a shape rather than with a level.
    ClockFace face;
    ClockInputs in = Sane();
    in.rssi = -30;  // whatever the radio last saw is not this icon's subject

    in.wifi = WifiIcon::kAp;
    ClockView view = face.Update(in);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(WifiIcon::kAp), static_cast<int>(view.wifi));
    TEST_ASSERT_EQUAL_UINT8(0, view.bars);

    in.wifi = WifiIcon::kOff;
    view = face.Update(in);
    TEST_ASSERT_EQUAL_UINT8(0, view.bars);
}

// --- The bus dot ---------------------------------------------------------

void test_no_server_configured_is_not_a_fault(void) {
    ClockFace face;
    ClockInputs in = Sane();
    in.bus_configured = false;
    in.bus_connected = false;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(BusIcon::kOff),
                          static_cast<int>(face.Update(in).bus));
}

void test_a_server_we_are_not_on_is_red(void) {
    ClockFace face;
    ClockInputs in = Sane();
    in.bus_configured = true;
    in.bus_connected = false;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(BusIcon::kDown),
                          static_cast<int>(face.Update(in).bus));
}

void test_a_quiet_connection_is_plain_green(void) {
    ClockFace face;
    ClockInputs in = Sane();
    in.bus_configured = true;
    in.bus_connected = true;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(BusIcon::kUp),
                          static_cast<int>(face.Update(in).bus));
}

void test_the_first_reading_of_the_counter_is_not_traffic(void) {
    // A device that has been up for an hour must not light the dot simply
    // because the screen was created and saw a non-zero count.
    ClockFace face;
    ClockInputs in = Sane();
    in.bus_configured = true;
    in.bus_connected = true;
    in.bus_messages = 4211;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(BusIcon::kUp),
                          static_cast<int>(face.Update(in).bus));
}

void test_a_message_lights_the_dot_and_it_goes_out_after_two_minutes(void) {
    ClockFace face;
    ClockInputs in = Sane();
    in.bus_configured = true;
    in.bus_connected = true;
    in.now_ms = 1000;
    in.bus_messages = 7;
    face.Update(in);  // the baseline

    in.now_ms = 2000;
    in.bus_messages = 8;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(BusIcon::kActive),
                          static_cast<int>(face.Update(in).bus));

    // Still inside the window one millisecond before it closes.
    in.now_ms = 2000 + ClockFace::kTrafficWindowMs - 1;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(BusIcon::kActive),
                          static_cast<int>(face.Update(in).bus));

    in.now_ms = 2000 + ClockFace::kTrafficWindowMs;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(BusIcon::kUp),
                          static_cast<int>(face.Update(in).bus));
}

void test_a_counter_that_went_backwards_is_a_reconnect_and_not_a_message(void) {
    // §10.5: the client object is destroyed and rebuilt on a reconnect, so its
    // counters restart at zero. That is the socket coming back, not a request
    // arriving, and a dot that lit for it would be lying about the one thing it
    // is for.
    ClockFace face;
    ClockInputs in = Sane();
    in.bus_configured = true;
    in.bus_connected = true;
    in.now_ms = 1000;
    in.bus_messages = 40;
    face.Update(in);

    in.now_ms = 2000;
    in.bus_messages = 0;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(BusIcon::kUp),
                          static_cast<int>(face.Update(in).bus));

    // …and the new baseline is the new counter, so the next real delivery does
    // light it.
    in.now_ms = 3000;
    in.bus_messages = 1;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(BusIcon::kActive),
                          static_cast<int>(face.Update(in).bus));
}

void test_traffic_is_not_shown_while_the_link_is_down(void) {
    // The dot's first job is the connection. A green-with-a-hole dot on a
    // dropped socket would say the loop is alive when it is not.
    ClockFace face;
    ClockInputs in = Sane();
    in.bus_configured = true;
    in.bus_connected = true;
    in.now_ms = 1000;
    in.bus_messages = 1;
    face.Update(in);

    in.now_ms = 2000;
    in.bus_messages = 2;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(BusIcon::kActive),
                          static_cast<int>(face.Update(in).bus));

    in.now_ms = 3000;
    in.bus_connected = false;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(BusIcon::kDown),
                          static_cast<int>(face.Update(in).bus));
}

void test_the_two_minute_window_survives_the_millisecond_wrap(void) {
    ClockFace face;
    ClockInputs in = Sane();
    in.bus_configured = true;
    in.bus_connected = true;

    in.now_ms = 0xFFFFF000u;
    in.bus_messages = 1;
    face.Update(in);

    in.now_ms = 0xFFFFF100u;
    in.bus_messages = 2;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(BusIcon::kActive),
                          static_cast<int>(face.Update(in).bus));

    // 60 s later, having gone through zero on the way.
    in.now_ms = 0xFFFFF100u + 60000u;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(BusIcon::kActive),
                          static_cast<int>(face.Update(in).bus));

    in.now_ms = 0xFFFFF100u + ClockFace::kTrafficWindowMs;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(BusIcon::kUp),
                          static_cast<int>(face.Update(in).bus));
}

// --- The battery ---------------------------------------------------------

void test_no_battery_reads_as_running_from_the_cable(void) {
    // The shipped board arrives with no cell on the MX1.25 connector, so this
    // is the ordinary case and not an error to report.
    ClockFace face;
    ClockInputs in = Sane();
    in.battery_present = false;
    in.vbus_present = true;
    in.battery_percent = -1;

    const ClockView view = face.Update(in);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(BatteryIcon::kAbsent),
                          static_cast<int>(view.battery));
    TEST_ASSERT_FALSE(view.battery_known);
}

void test_the_three_states_a_present_battery_can_be_in(void) {
    ClockFace face;
    ClockInputs in = Sane();
    in.battery_present = true;
    in.battery_percent = 62;

    in.vbus_present = false;
    in.charging = false;
    ClockView view = face.Update(in);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(BatteryIcon::kDischarging),
                          static_cast<int>(view.battery));
    TEST_ASSERT_TRUE(view.battery_known);
    TEST_ASSERT_EQUAL_UINT8(62, view.battery_percent);

    in.vbus_present = true;
    in.charging = true;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(BatteryIcon::kCharging),
                          static_cast<int>(face.Update(in).battery));

    // Cable in and nothing flowing: full, or the charger has finished. Not the
    // same picture as running the battery down.
    in.charging = false;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(BatteryIcon::kExternal),
                          static_cast<int>(face.Update(in).battery));
}

void test_a_percentage_outside_the_scale_is_clamped(void) {
    // The same call §10.8.3 makes about somebody else's `130`: a bar drawn from
    // it overflows its track.
    ClockFace face;
    ClockInputs in = Sane();
    in.battery_present = true;

    in.battery_percent = 130;
    TEST_ASSERT_EQUAL_UINT8(100, face.Update(in).battery_percent);

    in.battery_percent = 0;
    ClockView view = face.Update(in);
    TEST_ASSERT_EQUAL_UINT8(0, view.battery_percent);
    TEST_ASSERT_TRUE(view.battery_known);

    // Negative is "there is nothing to ask", which is a different answer from
    // zero and must not read as an empty battery.
    in.battery_percent = -1;
    view = face.Update(in);
    TEST_ASSERT_FALSE(view.battery_known);
}

// --- The drift (§10.8.1) -------------------------------------------------

void test_the_drift_stays_inside_its_box(void) {
    for (uint32_t ms = 0; ms < ClockFace::kCycleMs; ms += 37) {
        int16_t dx = 0;
        int16_t dy = 0;
        ClockFace::DriftAt(ms, &dx, &dy);
        TEST_ASSERT_GREATER_OR_EQUAL_INT16(-ClockFace::kDriftX, dx);
        TEST_ASSERT_LESS_OR_EQUAL_INT16(ClockFace::kDriftX, dx);
        TEST_ASSERT_GREATER_OR_EQUAL_INT16(-ClockFace::kDriftY, dy);
        TEST_ASSERT_LESS_OR_EQUAL_INT16(ClockFace::kDriftY, dy);
    }
}

void test_the_drift_visits_all_four_corners_of_it(void) {
    // A walk that stayed on one diagonal would satisfy the bounds test above
    // and would wear a line into the panel, which is the whole thing this
    // exists to avoid.
    bool corner[4] = {};
    for (uint32_t ms = 0; ms < ClockFace::kCycleMs; ms += 100) {
        int16_t dx = 0;
        int16_t dy = 0;
        ClockFace::DriftAt(ms, &dx, &dy);
        const int half_x = ClockFace::kDriftX / 2;
        const int half_y = ClockFace::kDriftY / 2;
        if (dx <= -half_x && dy <= -half_y) corner[0] = true;
        if (dx >= half_x && dy <= -half_y) corner[1] = true;
        if (dx <= -half_x && dy >= half_y) corner[2] = true;
        if (dx >= half_x && dy >= half_y) corner[3] = true;
    }
    for (int i = 0; i < 4; ++i) {
        TEST_ASSERT_TRUE_MESSAGE(corner[i], "the drift never reaches a corner");
    }
}

void test_the_drift_never_stops(void) {
    // Over any minute the face must have moved. A drift that had settled is a
    // drift that is no longer protecting anything.
    ClockFace face;
    ClockInputs in = Sane();
    int16_t seen_x = 0;
    int16_t seen_y = 0;
    bool first = true;

    for (uint32_t ms = 0; ms < ClockFace::kCycleMs; ms += 30000) {
        in.now_ms = ms;
        const ClockView view = face.Update(in);
        if (!first) {
            TEST_ASSERT_TRUE_MESSAGE(view.drift_x != seen_x || view.drift_y != seen_y,
                                     "the face stood still for half a minute");
        }
        seen_x = view.drift_x;
        seen_y = view.drift_y;
        first = false;
    }
}

void test_the_drift_is_continuous_across_the_millisecond_wrap(void) {
    // The reason `cycle_ms_` is accumulated rather than taken as
    // `now_ms % period`: the latter jumps by most of the box once every ~49
    // days, which on a desk object reads as the screen glitching.
    ClockFace face;
    ClockInputs in = Sane();

    in.now_ms = 0xFFFF0000u;
    ClockView previous = face.Update(in);

    for (uint32_t step = 1; step <= 2000; ++step) {
        in.now_ms = 0xFFFF0000u + step * 100u;  // walks through zero
        const ClockView view = face.Update(in);
        const int dx = view.drift_x - previous.drift_x;
        const int dy = view.drift_y - previous.drift_y;
        TEST_ASSERT_LESS_OR_EQUAL_INT(2, dx > 0 ? dx : -dx);
        TEST_ASSERT_LESS_OR_EQUAL_INT(2, dy > 0 ? dy : -dy);
        previous = view;
    }
}

void test_a_long_gap_between_updates_still_lands_in_the_box(void) {
    // The task can be starved — a Wi-Fi scan blocks for a second or two — and a
    // delta larger than one cycle must reduce rather than overflow.
    ClockFace face;
    ClockInputs in = Sane();
    in.now_ms = 0;
    face.Update(in);

    const uint32_t jumps[] = {ClockFace::kCycleMs + 5, 0x7FFFFFFFu, 0xFFFFFFF0u};
    uint32_t at = 0;
    for (uint32_t jump : jumps) {
        at += jump;
        in.now_ms = at;
        const ClockView view = face.Update(in);
        TEST_ASSERT_GREATER_OR_EQUAL_INT16(-ClockFace::kDriftX, view.drift_x);
        TEST_ASSERT_LESS_OR_EQUAL_INT16(ClockFace::kDriftX, view.drift_x);
        TEST_ASSERT_GREATER_OR_EQUAL_INT16(-ClockFace::kDriftY, view.drift_y);
        TEST_ASSERT_LESS_OR_EQUAL_INT16(ClockFace::kDriftY, view.drift_y);
    }
}

// --- The water -----------------------------------------------------------

void test_the_shimmer_never_reaches_either_extreme(void) {
    // The digits are lit by this and nothing else, so a trough of zero would
    // make them vanish in bands — and a crest of 255 is the pure green an AMOLED
    // pays for in lifetime. Both ends are held off deliberately, which is the
    // "not very bright" half of §10.8.1 living in one clamp.
    for (int16_t y = -64; y < 320; ++y) {
        for (int phase = 0; phase < 256; ++phase) {
            const uint8_t value = ClockFace::Shimmer(y, static_cast<uint8_t>(phase));
            TEST_ASSERT_GREATER_OR_EQUAL_UINT8(ClockFace::kShimmerFloor, value);
        }
    }
}

void test_the_shimmer_travels(void) {
    // At a fixed row, sweeping the phase has to cover a good part of the scale —
    // otherwise the water is a flat colour with a rounding error in it.
    uint8_t low = 255;
    uint8_t high = 0;
    for (int phase = 0; phase < 256; ++phase) {
        const uint8_t value = ClockFace::Shimmer(70, static_cast<uint8_t>(phase));
        if (value < low) low = value;
        if (value > high) high = value;
    }
    TEST_ASSERT_GREATER_THAN_INT(60, static_cast<int>(high) - static_cast<int>(low));
}

void test_the_shimmer_varies_down_the_face(void) {
    // And at a fixed phase, down the rows: a value that did not change with `y`
    // would be a flat fill that flickers rather than a texture that flows.
    uint8_t low = 255;
    uint8_t high = 0;
    for (int16_t y = 0; y < 150; ++y) {
        const uint8_t value = ClockFace::Shimmer(y, 0);
        if (value < low) low = value;
        if (value > high) high = value;
    }
    TEST_ASSERT_GREATER_THAN_INT(60, static_cast<int>(high) - static_cast<int>(low));
}

void test_the_wave_is_interpolated_rather_than_stepped(void) {
    // 64 table entries against 256 angles. Read flat, at most 64 distinct values
    // come out of it — fewer, because the table repeats a few — and the texture
    // built on it steps every four angle units. This is the assertion that pins
    // the interpolation, and it exists because the mutation pass showed that
    // breaking it was invisible through `Shimmer` (§10.11: a mutation that
    // survives is a question about the code, and the answer here was that the
    // property needed testing one level down).
    bool seen[256] = {};
    int distinct = 0;
    for (int angle = 0; angle < 256; ++angle) {
        const uint8_t value = ClockFace::Wave(static_cast<uint8_t>(angle));
        if (!seen[value]) {
            seen[value] = true;
            ++distinct;
        }
    }
    TEST_ASSERT_GREATER_THAN_INT(120, distinct);

    // And it is still a sine: one crest, one trough, and the ends meet.
    TEST_ASSERT_EQUAL_UINT8(255, ClockFace::Wave(64));
    TEST_ASSERT_EQUAL_UINT8(0, ClockFace::Wave(192));
    TEST_ASSERT_EQUAL_UINT8(ClockFace::Wave(0), ClockFace::Wave(128));
}

void test_the_shimmer_has_no_hard_edges_in_it(void) {
    // Adjacent rows must be close, or the "water" is a set of stripes with
    // visible boundaries — which is what a sine table read without interpolating
    // between its entries produces, and the reason `Wave` interpolates.
    for (int phase = 0; phase < 256; phase += 5) {
        for (int16_t y = 0; y < 200; ++y) {
            const int here = ClockFace::Shimmer(y, static_cast<uint8_t>(phase));
            const int next = ClockFace::Shimmer(static_cast<int16_t>(y + 1),
                                                static_cast<uint8_t>(phase));
            const int step = next > here ? next - here : here - next;
            TEST_ASSERT_LESS_OR_EQUAL_INT(16, step);
        }
    }
}

void test_the_phase_advances_evenly_and_wraps_without_a_jump(void) {
    // The static_asserts in the header say the periods divide the cycle; this
    // says the arithmetic built on them actually is continuous, all the way
    // round and through the millisecond wrap on the way.
    ClockFace face;
    ClockInputs in = Sane();
    in.now_ms = 0xFFFFF000u;
    uint8_t previous = face.Update(in).phase;

    const uint32_t step_ms = 100;
    const uint8_t expected = static_cast<uint8_t>(step_ms * 256 / ClockFace::kPhasePeriodMs);
    TEST_ASSERT_GREATER_THAN_UINT8(0, expected);

    for (uint32_t i = 1; i <= ClockFace::kCycleMs / step_ms + 10; ++i) {
        in.now_ms = 0xFFFFF000u + i * step_ms;
        const uint8_t phase = face.Update(in).phase;
        const uint8_t delta = static_cast<uint8_t>(phase - previous);
        TEST_ASSERT_EQUAL_UINT8(expected, delta);
        previous = phase;
    }
}

}  // namespace

void RegisterClockFaceTests(void) {
    RUN_TEST(test_an_unset_clock_shows_dashes_rather_than_midnight);
    RUN_TEST(test_a_year_before_the_rtc_can_hold_is_not_believed);
    RUN_TEST(test_a_year_past_the_rtc_is_refused_the_way_the_rtc_refuses_it);
    RUN_TEST(test_the_digits_are_twenty_four_hour);
    RUN_TEST(test_a_wall_clock_out_of_range_shows_dashes);

    RUN_TEST(test_bars_climb_with_the_signal);
    RUN_TEST(test_a_connected_link_never_shows_no_bars);
    RUN_TEST(test_a_client_link_reports_its_bars);
    RUN_TEST(test_connecting_animates_and_is_never_blank);
    RUN_TEST(test_the_access_point_and_the_radio_off_light_no_bars);

    RUN_TEST(test_no_server_configured_is_not_a_fault);
    RUN_TEST(test_a_server_we_are_not_on_is_red);
    RUN_TEST(test_a_quiet_connection_is_plain_green);
    RUN_TEST(test_the_first_reading_of_the_counter_is_not_traffic);
    RUN_TEST(test_a_message_lights_the_dot_and_it_goes_out_after_two_minutes);
    RUN_TEST(test_a_counter_that_went_backwards_is_a_reconnect_and_not_a_message);
    RUN_TEST(test_traffic_is_not_shown_while_the_link_is_down);
    RUN_TEST(test_the_two_minute_window_survives_the_millisecond_wrap);

    RUN_TEST(test_no_battery_reads_as_running_from_the_cable);
    RUN_TEST(test_the_three_states_a_present_battery_can_be_in);
    RUN_TEST(test_a_percentage_outside_the_scale_is_clamped);

    RUN_TEST(test_the_drift_stays_inside_its_box);
    RUN_TEST(test_the_drift_visits_all_four_corners_of_it);
    RUN_TEST(test_the_drift_never_stops);
    RUN_TEST(test_the_drift_is_continuous_across_the_millisecond_wrap);
    RUN_TEST(test_a_long_gap_between_updates_still_lands_in_the_box);

    RUN_TEST(test_the_shimmer_never_reaches_either_extreme);
    RUN_TEST(test_the_shimmer_travels);
    RUN_TEST(test_the_shimmer_varies_down_the_face);
    RUN_TEST(test_the_wave_is_interpolated_rather_than_stepped);
    RUN_TEST(test_the_shimmer_has_no_hard_edges_in_it);
    RUN_TEST(test_the_phase_advances_evenly_and_wraps_without_a_jump);
}
