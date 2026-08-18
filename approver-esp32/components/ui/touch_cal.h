#pragma once

// The touch correction and the flow that produces it (CLAUDE.md §10.8.5).
//
// `<cstdint>` and nothing else, like everything in `components/ui` — which is
// what puts the arithmetic under §10.11 rather than under a finger.
//
// **What a capacitive panel can and cannot need, because this is the first
// question and getting it wrong builds the wrong thing.** The CST9220 reports in
// its own native grid and that grid is 480×480 — the driver prints it at boot,
// the same numbers as the glass — so there is no gain to trim the way a resistive
// screen needs. What there *is*:
//
//   * **the axes**, which are `swap_xy` / `mirror_x` / `mirror_y` in `board.h`
//     and come from the vendor's example. They are not derivable from anything
//     (§10.1), and if a revision lays the film down differently they are simply
//     wrong. A negative scale below fixes a mirror, which is why the plausibility
//     check bounds the *magnitude* and not the sign;
//   * **a small offset**, from where the film sits over the glass. Real, and
//     usually a few pixels;
//   * **nothing else.** So this is one affine per axis and not a mesh: four
//     numbers, two of which are almost always 1.0.
//
// **The rule the whole design hangs off: a calibration must not be able to lock
// the operator out.** The screen it is reached from is touched, so a correction
// that puts every press in the wrong place would take away the way to fix it.
// Two answers, and both are needed:
//
//   * a fit that is not believable is **refused**, with a reason, and the old
//     one is kept. `FitTouch` below is where that happens, and every refusal is
//     its own value because each one is a different sentence on the glass;
//   * and the screen is drivable by the three physical buttons, so the escape
//     hatch does not go through the thing being calibrated.

#include <cstdint>

namespace ui {

// Fixed point: 1000 is 1.0. A `int16_t` then holds ±32, and the plausibility
// bound below is ±2 — the room is for arithmetic, not for a scale anybody wants.
inline constexpr int16_t kTouchScaleOne = 1000;

// One corner each. Four is the smallest number that over-determines two
// parameters per axis, which is what makes the fit a least squares rather than
// two subtractions — a single bad tap then moves the answer a little instead of
// deciding it.
inline constexpr uint8_t kTouchTargets = 4;

// How far in from the edge the crosses sit. Far enough that a fingertip is not
// half off the glass, close enough that the span is most of the panel — the span
// is the denominator of the scale, so a small one is a noisy answer.
inline constexpr int16_t kTouchInset = 64;

struct TouchCalibration {
    int16_t scale_x = kTouchScaleOne;
    int16_t scale_y = kTouchScaleOne;
    int16_t offset_x = 0;
    int16_t offset_y = 0;

    // No correction at all — what a device that has never been calibrated has,
    // and what the reset on the screen puts back. Worth a name because "is this
    // device calibrated" is a question three different readouts ask.
    bool Identity() const {
        return scale_x == kTouchScaleOne && scale_y == kTouchScaleOne && offset_x == 0 &&
               offset_y == 0;
    }

    // Raw controller coordinates in, screen coordinates out, **clamped to the
    // panel**. The clamp is not tidiness: a point off the edge is a point LVGL
    // would hit-test against nothing, so a correction slightly too aggressive
    // would make the last row of a list unpressable rather than slightly off.
    void Apply(uint16_t width, uint16_t height, uint16_t *x, uint16_t *y) const;
};

// One cross, and where the finger actually landed.
struct TouchSample {
    int16_t target_x = 0;
    int16_t target_y = 0;
    int16_t raw_x = 0;
    int16_t raw_y = 0;
};

// Why a fit was refused. **Each one is its own value because each one is its own
// sentence**: "you tapped the same place four times" and "your screen is mounted
// sideways" are different problems, and a single `false` would send somebody
// hunting the wrong one.
enum class TouchFit : uint8_t {
    kOk = 0,
    kNotEnough,         // fewer than `kTouchTargets` points
    kTargetsDegenerate, // the crosses themselves do not span the panel — a caller bug
    kSpanTooSmall,      // the taps do not: four presses in one place
    kScaleImplausible,  // |scale| outside [0.5, 2.0]
    kOffScreen,         // the corners would not land on the glass
};

const char *TouchFitText(TouchFit fit);

// Where the crosses go, for a panel of this size. Static and shared so that the
// screen that draws them and the fit that consumes them cannot disagree about
// where the operator was asked to press.
void TouchTarget(uint8_t index, uint16_t width, uint16_t height, int16_t *x, int16_t *y);

// Least squares per axis, then every refusal above. `out` is left **untouched**
// on anything but `kOk` — the caller's calibration is the one in use, and a
// half-written one is a screen nobody can fix.
TouchFit FitTouch(const TouchSample *samples, uint8_t count, uint16_t width, uint16_t height,
                  TouchCalibration *out);

// --- The flow ------------------------------------------------------------

enum class TouchStage : uint8_t {
    kTest = 0,   // a crosshair follows the finger; nothing is being changed
    kCollecting, // one cross at a time
    kResult,     // what the fit said, for a beat
};

class TouchFlow {
   public:
    // A press shorter than this is not a tap: the panel reports a stray point
    // now and then, and one of them landing in a calibration is a correction
    // built out of noise.
    static constexpr uint32_t kMinPressMs = 80;

    // And one longer than this is a finger resting on the glass rather than
    // pointing at a cross. Refused rather than taken, because a long press
    // drifts and the point recorded is wherever it ended up.
    static constexpr uint32_t kMaxPressMs = 4000;

    // How long the outcome stays before the screen goes back to the test.
    static constexpr uint32_t kResultMs = 3000;

    TouchFlow() = default;
    TouchFlow(const TouchFlow &) = delete;
    TouchFlow &operator=(const TouchFlow &) = delete;

    TouchStage Stage() const { return stage_; }
    uint8_t Collected() const { return collected_; }
    TouchFit Outcome() const { return outcome_; }

    // Back to the test with nothing collected. What opening the screen does, and
    // what `PWR` does part-way through — an abandoned calibration must leave the
    // one in use exactly as it was.
    void Reset();

    void Start();

    // A finger came up. `held_ms` is how long it was down and `raw` is where it
    // was; returns true if the point was taken. Ignored outside `kCollecting`,
    // which is what makes the test mode safe to poke at.
    bool Released(int16_t raw_x, int16_t raw_y, uint32_t held_ms);

    // Once every point is in: fit, record the outcome, and go to `kResult`.
    // Returns the outcome, and writes `out` only on success.
    TouchFit Finish(uint16_t width, uint16_t height, TouchCalibration *out, uint32_t now_ms);

    // Lets the result fade back to the test. True when it just did.
    bool Tick(uint32_t now_ms);

   private:
    TouchStage stage_ = TouchStage::kTest;
    uint8_t collected_ = 0;
    TouchFit outcome_ = TouchFit::kOk;
    uint32_t result_at_ms_ = 0;
    TouchSample samples_[kTouchTargets] = {};
};

}  // namespace ui
