// The touch correction and the flow that produces it (CLAUDE.md §10.8.5),
// tested where they cost nothing to test (§10.11, host tier).
//
// `touch_cal.h` includes `<cstdint>` and nothing else, so this suite needs no
// fake — the navigator's shape rather than the drivers'.
//
// **Nearly all of the value is in the refusals.** A correction that is applied
// is a correction the operator can see and redo; one that is *wrong* takes away
// the screen it was reached from, and the buttons are the only way back. So
// every way a fit can be nonsense has its own case here, and each one is
// asserted to leave the caller's calibration exactly as it was.

#include <cstring>

#include "touch_cal.h"
#include "unity.h"

using ui::kTouchInset;
using ui::kTouchScaleOne;
using ui::kTouchTargets;
using ui::TouchCalibration;
using ui::TouchFit;
using ui::TouchFlow;
using ui::TouchSample;
using ui::TouchStage;

namespace {

constexpr uint16_t kW = 480;
constexpr uint16_t kH = 480;

// A panel whose controller agrees with its glass: the raw point is the screen
// point, which is what a correctly mounted CST9220 gives and what an
// uncalibrated device is assumed to have.
void PerfectSamples(TouchSample *out) {
    for (uint8_t i = 0; i < kTouchTargets; ++i) {
        int16_t x = 0;
        int16_t y = 0;
        ui::TouchTarget(i, kW, kH, &x, &y);
        out[i].target_x = x;
        out[i].target_y = y;
        out[i].raw_x = x;
        out[i].raw_y = y;
    }
}

// The same, with the raw points moved by an affine the fit should recover.
void ShiftedSamples(TouchSample *out, int16_t dx, int16_t dy) {
    PerfectSamples(out);
    for (uint8_t i = 0; i < kTouchTargets; ++i) {
        out[i].raw_x = static_cast<int16_t>(out[i].raw_x + dx);
        out[i].raw_y = static_cast<int16_t>(out[i].raw_y + dy);
    }
}

}  // namespace

// --- The correction itself -----------------------------------------------

void test_an_uncalibrated_device_changes_nothing(void) {
    TouchCalibration none;
    TEST_ASSERT_TRUE(none.Identity());

    uint16_t x = 137;
    uint16_t y = 401;
    none.Apply(kW, kH, &x, &y);
    TEST_ASSERT_EQUAL_UINT16(137, x);
    TEST_ASSERT_EQUAL_UINT16(401, y);
}

void test_the_correction_shifts_and_scales(void) {
    TouchCalibration cal;
    cal.scale_x = kTouchScaleOne;
    cal.offset_x = -20;
    cal.scale_y = 2 * kTouchScaleOne;
    cal.offset_y = 0;
    TEST_ASSERT_FALSE(cal.Identity());

    uint16_t x = 100;
    uint16_t y = 100;
    cal.Apply(kW, kH, &x, &y);
    TEST_ASSERT_EQUAL_UINT16(80, x);
    TEST_ASSERT_EQUAL_UINT16(200, y);
}

void test_a_corrected_point_is_clamped_onto_the_panel(void) {
    // Not tidiness: a point off the edge is a point LVGL hit-tests against
    // nothing, so a slightly over-eager correction would make the last row of a
    // list unpressable rather than a few pixels out.
    TouchCalibration cal;
    cal.offset_x = -400;
    cal.offset_y = 400;

    uint16_t x = 10;
    uint16_t y = 470;
    cal.Apply(kW, kH, &x, &y);
    TEST_ASSERT_EQUAL_UINT16(0, x);
    TEST_ASSERT_EQUAL_UINT16(kH - 1, y);
}

void test_a_mirrored_axis_is_something_the_correction_can_express(void) {
    // A negative scale is how a film laid down the other way round is undone —
    // which is why the plausibility check below bounds the magnitude and not
    // the sign.
    TouchCalibration cal;
    cal.scale_x = -kTouchScaleOne;
    cal.offset_x = kW - 1;

    uint16_t x = 0;
    uint16_t y = 240;
    cal.Apply(kW, kH, &x, &y);
    TEST_ASSERT_EQUAL_UINT16(kW - 1, x);

    x = kW - 1;
    cal.Apply(kW, kH, &x, &y);
    TEST_ASSERT_EQUAL_UINT16(0, x);
}

// --- The fit -------------------------------------------------------------

void test_a_panel_that_needs_nothing_fits_to_nothing(void) {
    TouchSample samples[kTouchTargets] = {};
    PerfectSamples(samples);

    TouchCalibration cal;
    cal.offset_x = 99;  // scribble, so a fit that writes nothing is visible
    TEST_ASSERT_EQUAL_INT(static_cast<int>(TouchFit::kOk),
                          static_cast<int>(ui::FitTouch(samples, kTouchTargets, kW, kH, &cal)));
    TEST_ASSERT_TRUE(cal.Identity());
}

void test_a_shifted_panel_fits_to_the_shift(void) {
    // The ordinary case: the film sits a few pixels off the glass, so every
    // press lands the same distance from where it was aimed.
    TouchSample samples[kTouchTargets] = {};
    ShiftedSamples(samples, 12, -7);

    TouchCalibration cal;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(TouchFit::kOk),
                          static_cast<int>(ui::FitTouch(samples, kTouchTargets, kW, kH, &cal)));
    TEST_ASSERT_EQUAL_INT16(kTouchScaleOne, cal.scale_x);
    TEST_ASSERT_EQUAL_INT16(kTouchScaleOne, cal.scale_y);
    TEST_ASSERT_EQUAL_INT16(-12, cal.offset_x);
    TEST_ASSERT_EQUAL_INT16(7, cal.offset_y);

    // And what matters is that it undoes the shift, not what the numbers are.
    uint16_t x = static_cast<uint16_t>(240 + 12);
    uint16_t y = static_cast<uint16_t>(240 - 7);
    cal.Apply(kW, kH, &x, &y);
    TEST_ASSERT_EQUAL_UINT16(240, x);
    TEST_ASSERT_EQUAL_UINT16(240, y);
}

void test_one_bad_tap_moves_the_answer_rather_than_deciding_it(void) {
    // Why it is a least squares over four points and not two subtractions: a
    // finger that slipped on one cross should cost a few pixels everywhere, not
    // the whole calibration.
    TouchSample samples[kTouchTargets] = {};
    PerfectSamples(samples);
    samples[1].raw_x = static_cast<int16_t>(samples[1].raw_x + 40);

    TouchCalibration cal;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(TouchFit::kOk),
                          static_cast<int>(ui::FitTouch(samples, kTouchTargets, kW, kH, &cal)));

    uint16_t x = 240;
    uint16_t y = 240;
    cal.Apply(kW, kH, &x, &y);
    const int drift = static_cast<int>(x) - 240;
    TEST_ASSERT_TRUE_MESSAGE(drift > -25 && drift < 25, "one bad tap decided the fit");
}

void test_a_mirrored_panel_fits_to_a_negative_scale(void) {
    TouchSample samples[kTouchTargets] = {};
    PerfectSamples(samples);
    for (uint8_t i = 0; i < kTouchTargets; ++i) {
        samples[i].raw_x = static_cast<int16_t>(kW - 1 - samples[i].raw_x);
    }

    TouchCalibration cal;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(TouchFit::kOk),
                          static_cast<int>(ui::FitTouch(samples, kTouchTargets, kW, kH, &cal)));
    TEST_ASSERT_TRUE(cal.scale_x < 0);

    uint16_t x = static_cast<uint16_t>(kW - 1 - 100);
    uint16_t y = 240;
    cal.Apply(kW, kH, &x, &y);
    TEST_ASSERT_INT_WITHIN(2, 100, x);
}

// --- Every refusal, each with its own sentence ---------------------------

void test_a_fit_with_missing_points_is_refused(void) {
    TouchSample samples[kTouchTargets] = {};
    PerfectSamples(samples);

    TouchCalibration cal;
    cal.offset_x = 42;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(TouchFit::kNotEnough),
                          static_cast<int>(ui::FitTouch(samples, kTouchTargets - 1, kW, kH, &cal)));
    TEST_ASSERT_EQUAL_INT16(42, cal.offset_x);
}

void test_four_taps_in_one_place_are_refused(void) {
    TouchSample samples[kTouchTargets] = {};
    PerfectSamples(samples);
    for (uint8_t i = 0; i < kTouchTargets; ++i) {
        samples[i].raw_x = 240;
        samples[i].raw_y = 240;
    }

    TouchCalibration cal;
    cal.offset_y = 7;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(TouchFit::kSpanTooSmall),
                          static_cast<int>(ui::FitTouch(samples, kTouchTargets, kW, kH, &cal)));
    TEST_ASSERT_EQUAL_INT16(7, cal.offset_y);
}

void test_a_stretch_nobody_could_want_is_refused(void) {
    // The raw points span a quarter of what the crosses do, which is a scale of
    // four. Bounded at two — and asserted as **exactly** that refusal, because
    // the looser version of this test is what let the bound go unreached: the
    // span guard next to it used to fire first, and a test that accepted either
    // answer could not tell.
    TouchSample samples[kTouchTargets] = {};
    PerfectSamples(samples);
    for (uint8_t i = 0; i < kTouchTargets; ++i) {
        samples[i].raw_x = static_cast<int16_t>(240 + (samples[i].raw_x - 240) / 4);
        samples[i].raw_y = static_cast<int16_t>(240 + (samples[i].raw_y - 240) / 4);
    }

    TouchCalibration cal;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(TouchFit::kScaleImplausible),
                          static_cast<int>(ui::FitTouch(samples, kTouchTargets, kW, kH, &cal)));
    TEST_ASSERT_TRUE(cal.Identity());
}

void test_presses_that_barely_moved_are_refused_as_a_span_and_not_a_stretch(void) {
    // The other side of the same boundary: near enough to one place that the
    // fit through them means nothing, and the operator is told *that* rather
    // than being told the screen would stretch.
    //
    // **The divisor is chosen, not arbitrary.** Squeeze them harder and the fit
    // overflows an `int16_t` scale and is refused by the arithmetic instead —
    // which is the right outcome and the wrong *test*, because it would pass
    // with the span guard deleted. A twentieth leaves a scale of about 22, well
    // inside the type, so this guard is the only thing that can refuse it.
    TouchSample samples[kTouchTargets] = {};
    PerfectSamples(samples);
    for (uint8_t i = 0; i < kTouchTargets; ++i) {
        samples[i].raw_x = static_cast<int16_t>(240 + (samples[i].raw_x - 240) / 20);
        samples[i].raw_y = static_cast<int16_t>(240 + (samples[i].raw_y - 240) / 20);
    }

    TouchCalibration cal;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(TouchFit::kSpanTooSmall),
                          static_cast<int>(ui::FitTouch(samples, kTouchTargets, kW, kH, &cal)));
    TEST_ASSERT_TRUE(cal.Identity());
}

void test_a_fit_that_would_push_a_corner_off_the_glass_is_refused(void) {
    // A believable scale with an unbelievable offset: every press lands, none of
    // them anywhere near the finger.
    TouchSample samples[kTouchTargets] = {};
    ShiftedSamples(samples, 300, 0);

    TouchCalibration cal;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(TouchFit::kOffScreen),
                          static_cast<int>(ui::FitTouch(samples, kTouchTargets, kW, kH, &cal)));
    TEST_ASSERT_TRUE(cal.Identity());
}

void test_every_refusal_has_its_own_sentence(void) {
    // Because "you tapped the same place four times" and "your screen is mounted
    // sideways" are different problems, and one `false` would send somebody
    // hunting the wrong one.
    const TouchFit every[] = {TouchFit::kOk,           TouchFit::kNotEnough,
                              TouchFit::kTargetsDegenerate, TouchFit::kSpanTooSmall,
                              TouchFit::kScaleImplausible, TouchFit::kOffScreen};
    for (TouchFit a : every) {
        TEST_ASSERT_NOT_NULL(ui::TouchFitText(a));
        for (TouchFit b : every) {
            if (a == b) {
                continue;
            }
            TEST_ASSERT_NOT_EQUAL(0, std::strcmp(ui::TouchFitText(a), ui::TouchFitText(b)));
        }
    }
}

// --- The crosses ---------------------------------------------------------

void test_the_crosses_are_four_different_corners_inside_the_glass(void) {
    int16_t xs[kTouchTargets] = {};
    int16_t ys[kTouchTargets] = {};
    for (uint8_t i = 0; i < kTouchTargets; ++i) {
        ui::TouchTarget(i, kW, kH, &xs[i], &ys[i]);
        TEST_ASSERT_TRUE(xs[i] >= kTouchInset && xs[i] <= static_cast<int16_t>(kW) - kTouchInset);
        TEST_ASSERT_TRUE(ys[i] >= kTouchInset && ys[i] <= static_cast<int16_t>(kH) - kTouchInset);
    }
    for (uint8_t i = 0; i < kTouchTargets; ++i) {
        for (uint8_t j = i + 1; j < kTouchTargets; ++j) {
            TEST_ASSERT_FALSE(xs[i] == xs[j] && ys[i] == ys[j]);
        }
    }

    // Consecutive crosses are never on top of each other, so a finger that did
    // not move cannot look like two different presses.
    for (uint8_t i = 1; i < kTouchTargets; ++i) {
        TEST_ASSERT_FALSE(xs[i] == xs[i - 1] && ys[i] == ys[i - 1]);
    }
}

// --- The flow ------------------------------------------------------------

void test_the_screen_opens_on_the_test_and_changes_nothing(void) {
    TouchFlow flow;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(TouchStage::kTest), static_cast<int>(flow.Stage()));
    TEST_ASSERT_FALSE(flow.Released(100, 100, 200));
    TEST_ASSERT_EQUAL_UINT8(0, flow.Collected());
}

void test_a_press_too_short_or_too_long_is_not_a_point(void) {
    TouchFlow flow;
    flow.Start();
    TEST_ASSERT_FALSE(flow.Released(64, 64, TouchFlow::kMinPressMs - 1));
    TEST_ASSERT_FALSE(flow.Released(64, 64, TouchFlow::kMaxPressMs + 1));
    TEST_ASSERT_EQUAL_UINT8(0, flow.Collected());

    TEST_ASSERT_TRUE(flow.Released(64, 64, TouchFlow::kMinPressMs));
    TEST_ASSERT_EQUAL_UINT8(1, flow.Collected());
}

void test_a_finished_set_fits_and_shows_the_outcome(void) {
    TouchFlow flow;
    flow.Start();
    for (uint8_t i = 0; i < kTouchTargets; ++i) {
        int16_t x = 0;
        int16_t y = 0;
        ui::TouchTarget(i, kW, kH, &x, &y);
        TEST_ASSERT_TRUE(flow.Released(x, y, 200));
    }
    TEST_ASSERT_EQUAL_UINT8(kTouchTargets, flow.Collected());

    TouchCalibration cal;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(TouchFit::kOk),
                          static_cast<int>(flow.Finish(kW, kH, &cal, 1000)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(TouchStage::kResult), static_cast<int>(flow.Stage()));
    TEST_ASSERT_TRUE(cal.Identity());
}

void test_a_fifth_press_is_ignored_rather_than_overwriting_a_point(void) {
    TouchFlow flow;
    flow.Start();
    for (uint8_t i = 0; i < kTouchTargets; ++i) {
        flow.Released(static_cast<int16_t>(64 + i * 100), 64, 200);
    }
    TEST_ASSERT_FALSE(flow.Released(1, 1, 200));
    TEST_ASSERT_EQUAL_UINT8(kTouchTargets, flow.Collected());
}

void test_the_result_fades_back_to_the_test(void) {
    TouchFlow flow;
    flow.Start();
    TouchCalibration cal;
    flow.Finish(kW, kH, &cal, 1000);

    TEST_ASSERT_FALSE(flow.Tick(1000 + TouchFlow::kResultMs - 1));
    TEST_ASSERT_TRUE(flow.Tick(1000 + TouchFlow::kResultMs));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(TouchStage::kTest), static_cast<int>(flow.Stage()));
}

void test_the_result_window_survives_the_millisecond_wrap(void) {
    TouchFlow flow;
    flow.Start();
    TouchCalibration cal;
    flow.Finish(kW, kH, &cal, 0xFFFFFF00u);

    TEST_ASSERT_FALSE(flow.Tick(0xFFFFFF10u));  // 16 ms later, not yet wrapped
    TEST_ASSERT_FALSE(flow.Tick(400));          // wrapped, 656 ms after
    TEST_ASSERT_TRUE(flow.Tick(0xFFFFFF00u + TouchFlow::kResultMs));
}

void test_abandoning_a_calibration_leaves_the_one_in_use_alone(void) {
    // What `PWR` does part-way through, and it is the safety property: a
    // calibration nobody finished must not touch the one the screen is being
    // driven with.
    TouchFlow flow;
    flow.Start();
    flow.Released(64, 64, 200);
    flow.Released(416, 64, 200);
    flow.Reset();

    TEST_ASSERT_EQUAL_INT(static_cast<int>(TouchStage::kTest), static_cast<int>(flow.Stage()));
    TEST_ASSERT_EQUAL_UINT8(0, flow.Collected());

    TouchCalibration cal;
    cal.offset_x = 33;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(TouchFit::kNotEnough),
                          static_cast<int>(flow.Finish(kW, kH, &cal, 2000)));
    TEST_ASSERT_EQUAL_INT16(33, cal.offset_x);
}

void RegisterTouchCalTests(void) {
    RUN_TEST(test_an_uncalibrated_device_changes_nothing);
    RUN_TEST(test_the_correction_shifts_and_scales);
    RUN_TEST(test_a_corrected_point_is_clamped_onto_the_panel);
    RUN_TEST(test_a_mirrored_axis_is_something_the_correction_can_express);

    RUN_TEST(test_a_panel_that_needs_nothing_fits_to_nothing);
    RUN_TEST(test_a_shifted_panel_fits_to_the_shift);
    RUN_TEST(test_one_bad_tap_moves_the_answer_rather_than_deciding_it);
    RUN_TEST(test_a_mirrored_panel_fits_to_a_negative_scale);

    RUN_TEST(test_a_fit_with_missing_points_is_refused);
    RUN_TEST(test_four_taps_in_one_place_are_refused);
    RUN_TEST(test_a_stretch_nobody_could_want_is_refused);
    RUN_TEST(test_presses_that_barely_moved_are_refused_as_a_span_and_not_a_stretch);
    RUN_TEST(test_a_fit_that_would_push_a_corner_off_the_glass_is_refused);
    RUN_TEST(test_every_refusal_has_its_own_sentence);

    RUN_TEST(test_the_crosses_are_four_different_corners_inside_the_glass);

    RUN_TEST(test_the_screen_opens_on_the_test_and_changes_nothing);
    RUN_TEST(test_a_press_too_short_or_too_long_is_not_a_point);
    RUN_TEST(test_a_finished_set_fits_and_shows_the_outcome);
    RUN_TEST(test_a_fifth_press_is_ignored_rather_than_overwriting_a_point);
    RUN_TEST(test_the_result_fades_back_to_the_test);
    RUN_TEST(test_the_result_window_survives_the_millisecond_wrap);
    RUN_TEST(test_abandoning_a_calibration_leaves_the_one_in_use_alone);
}
