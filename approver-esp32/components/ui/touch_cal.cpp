#include "touch_cal.h"

namespace ui {
namespace {

// The bounds a believable scale sits in. Half and double: a panel whose axes are
// right needs about 1.0, one that is mirrored needs about -1.0, and anything
// outside this is a set of taps that did not mean what the screen asked for.
constexpr int32_t kScaleMin = kTouchScaleOne / 2;
constexpr int32_t kScaleMax = kTouchScaleOne * 2;

// How far outside the panel a corrected corner may land before the fit is called
// unusable. Not zero: a few pixels of overhang is an ordinary calibration and the
// clamp in `Apply` handles it. A quarter of the panel is not.
constexpr int32_t kOffScreenSlack = 120;

// Rounded rather than truncated, and towards zero-or-away correctly for negative
// values — a scale can be negative when the axis is mirrored, and truncation
// there biases every point in one direction.
int32_t DivRound(int32_t numerator, int32_t denominator) {
    if (denominator == 0) {
        return 0;
    }
    const int32_t half = denominator / 2;
    return (numerator >= 0) == (denominator > 0) ? (numerator + half) / denominator
                                                 : (numerator - half) / denominator;
}

int32_t Clamp(int32_t value, int32_t low, int32_t high) {
    if (value < low) {
        return low;
    }
    return value > high ? high : value;
}

// One axis of the least squares. `raw` and `target` are four points each.
//
// **`int64_t` on purpose, and the honest version of why**: the numerator is a
// sum of products scaled by a thousand, and its worst case at 480 pixels and
// four points is about 3.7e9 — past what an `int32_t` holds, where it would wrap
// into a scale with the wrong sign. The *reachable* cases with the crosses this
// firmware draws stay well inside 32 bits, so this is defence rather than a
// measured overflow; it costs a handful of instructions in a function that runs
// four times in the life of a calibration, and the alternative is a bound
// somebody has to re-derive every time the crosses move.
bool FitAxis(const int16_t *raw, const int16_t *target, uint8_t count, int16_t *scale,
             int16_t *offset) {
    int64_t sum_r = 0;
    int64_t sum_t = 0;
    int64_t sum_rt = 0;
    int64_t sum_rr = 0;
    for (uint8_t i = 0; i < count; ++i) {
        const int64_t r = raw[i];
        const int64_t t = target[i];
        sum_r += r;
        sum_t += t;
        sum_rt += r * t;
        sum_rr += r * r;
    }

    const int64_t n = count;
    const int64_t denominator = n * sum_rr - sum_r * sum_r;
    if (denominator == 0) {
        // Every point on the same coordinate: there is no line through them.
        return false;
    }

    const int64_t numerator = (n * sum_rt - sum_r * sum_t) * kTouchScaleOne;
    const int64_t half = denominator / 2;
    const int64_t a = (numerator >= 0) == (denominator > 0) ? (numerator + half) / denominator
                                                            : (numerator - half) / denominator;

    // b = (sum_t - a*sum_r) / n, with `a` still scaled by a thousand.
    const int64_t b_scaled = sum_t * kTouchScaleOne - a * sum_r;
    const int64_t b_den = n * kTouchScaleOne;
    const int64_t b_half = b_den / 2;
    const int64_t b = b_scaled >= 0 ? (b_scaled + b_half) / b_den : (b_scaled - b_half) / b_den;

    if (a < -32768 || a > 32767 || b < -32768 || b > 32767) {
        return false;
    }
    *scale = static_cast<int16_t>(a);
    *offset = static_cast<int16_t>(b);
    return true;
}

int32_t Mapped(int16_t raw, int16_t scale, int16_t offset) {
    return DivRound(static_cast<int32_t>(raw) * scale, kTouchScaleOne) + offset;
}

}  // namespace

void TouchCalibration::Apply(uint16_t width, uint16_t height, uint16_t *x, uint16_t *y) const {
    if (x == nullptr || y == nullptr) {
        return;
    }
    const int32_t mapped_x = Mapped(static_cast<int16_t>(*x), scale_x, offset_x);
    const int32_t mapped_y = Mapped(static_cast<int16_t>(*y), scale_y, offset_y);
    *x = static_cast<uint16_t>(Clamp(mapped_x, 0, static_cast<int32_t>(width) - 1));
    *y = static_cast<uint16_t>(Clamp(mapped_y, 0, static_cast<int32_t>(height) - 1));
}

const char *TouchFitText(TouchFit fit) {
    switch (fit) {
        case TouchFit::kOk:
            return "calibrated";
        case TouchFit::kNotEnough:
            return "not every cross was pressed";
        case TouchFit::kTargetsDegenerate:
            return "the crosses do not span the panel";
        case TouchFit::kSpanTooSmall:
            return "the presses were all in one place";
        case TouchFit::kScaleImplausible:
            return "that would stretch the screen";
        case TouchFit::kOffScreen:
            return "that would push a corner off the glass";
    }
    return "refused";
}

void TouchTarget(uint8_t index, uint16_t width, uint16_t height, int16_t *x, int16_t *y) {
    if (x == nullptr || y == nullptr) {
        return;
    }
    const int16_t left = kTouchInset;
    const int16_t right = static_cast<int16_t>(width) - kTouchInset;
    const int16_t top = kTouchInset;
    const int16_t bottom = static_cast<int16_t>(height) - kTouchInset;

    // Round the panel rather than across it: consecutive crosses are never on
    // top of each other, so a finger that did not move cannot look like two
    // different presses.
    switch (index % kTouchTargets) {
        case 0:
            *x = left;
            *y = top;
            break;
        case 1:
            *x = right;
            *y = top;
            break;
        case 2:
            *x = right;
            *y = bottom;
            break;
        default:
            *x = left;
            *y = bottom;
            break;
    }
}

TouchFit FitTouch(const TouchSample *samples, uint8_t count, uint16_t width, uint16_t height,
                  TouchCalibration *out) {
    if (samples == nullptr || out == nullptr || count < kTouchTargets) {
        return TouchFit::kNotEnough;
    }

    int16_t raw_x[kTouchTargets] = {};
    int16_t raw_y[kTouchTargets] = {};
    int16_t target_x[kTouchTargets] = {};
    int16_t target_y[kTouchTargets] = {};

    int32_t target_span_x = 0;
    int32_t target_span_y = 0;
    int32_t raw_span_x = 0;
    int32_t raw_span_y = 0;

    for (uint8_t i = 0; i < kTouchTargets; ++i) {
        raw_x[i] = samples[i].raw_x;
        raw_y[i] = samples[i].raw_y;
        target_x[i] = samples[i].target_x;
        target_y[i] = samples[i].target_y;
        for (uint8_t j = 0; j < i; ++j) {
            const int32_t dx = target_x[i] - target_x[j];
            const int32_t dy = target_y[i] - target_y[j];
            const int32_t rx = raw_x[i] - raw_x[j];
            const int32_t ry = raw_y[i] - raw_y[j];
            target_span_x = dx < 0 ? (-dx > target_span_x ? -dx : target_span_x)
                                   : (dx > target_span_x ? dx : target_span_x);
            target_span_y = dy < 0 ? (-dy > target_span_y ? -dy : target_span_y)
                                   : (dy > target_span_y ? dy : target_span_y);
            raw_span_x = rx < 0 ? (-rx > raw_span_x ? -rx : raw_span_x)
                                : (rx > raw_span_x ? rx : raw_span_x);
            raw_span_y = ry < 0 ? (-ry > raw_span_y ? -ry : raw_span_y)
                                : (ry > raw_span_y ? ry : raw_span_y);
        }
    }

    // A caller that asked for four crosses in a line has a bug of its own, and
    // it is worth its own answer: the operator did nothing wrong.
    if (target_span_x < static_cast<int32_t>(width) / 4 ||
        target_span_y < static_cast<int32_t>(height) / 4) {
        return TouchFit::kTargetsDegenerate;
    }

    // **The one that catches a real operator mistake**: four taps in the same
    // place. The line through them is vertical, the scale is enormous, and
    // without this the refusal below would name the wrong problem.
    //
    // **An eighth, and the number came out of a mutation that survived.** It was
    // a half, which is the same statement as "the scale is at most two" — so it
    // fired first every time and the plausibility check below was unreachable
    // code that looked like a safety net. Two guards, two jobs: this one says
    // the presses were not spread out enough to mean anything, and the one below
    // says the fit through them is a stretch nobody wants.
    if (raw_span_x * 8 < target_span_x || raw_span_y * 8 < target_span_y) {
        return TouchFit::kSpanTooSmall;
    }

    TouchCalibration fitted;
    if (!FitAxis(raw_x, target_x, kTouchTargets, &fitted.scale_x, &fitted.offset_x) ||
        !FitAxis(raw_y, target_y, kTouchTargets, &fitted.scale_y, &fitted.offset_y)) {
        return TouchFit::kSpanTooSmall;
    }

    // Magnitude, not sign: a mirrored axis is a negative scale and is exactly
    // the thing this is allowed to fix (`touch_cal.h` says why).
    //
    // **The lower half of this bound is unreachable and is kept anyway**, which
    // is worth saying rather than leaving somebody to work out: a scale below
    // 0.5 needs the raw points to span more than twice the crosses, and the raw
    // points come off a controller whose grid is the panel. It costs one
    // comparison and it stops the check being a statement about today's
    // geometry.
    const int32_t abs_x = fitted.scale_x < 0 ? -fitted.scale_x : fitted.scale_x;
    const int32_t abs_y = fitted.scale_y < 0 ? -fitted.scale_y : fitted.scale_y;
    if (abs_x < kScaleMin || abs_x > kScaleMax || abs_y < kScaleMin || abs_y > kScaleMax) {
        return TouchFit::kScaleImplausible;
    }

    // And the last check is the one that means something to a person: run the
    // panel's own corners through it and see whether they still land on it.
    const int16_t corners[2] = {0, static_cast<int16_t>(width - 1)};
    const int16_t rows[2] = {0, static_cast<int16_t>(height - 1)};
    for (int16_t corner : corners) {
        const int32_t mapped = Mapped(corner, fitted.scale_x, fitted.offset_x);
        if (mapped < -kOffScreenSlack || mapped > static_cast<int32_t>(width) + kOffScreenSlack) {
            return TouchFit::kOffScreen;
        }
    }
    for (int16_t row : rows) {
        const int32_t mapped = Mapped(row, fitted.scale_y, fitted.offset_y);
        if (mapped < -kOffScreenSlack || mapped > static_cast<int32_t>(height) + kOffScreenSlack) {
            return TouchFit::kOffScreen;
        }
    }

    *out = fitted;
    return TouchFit::kOk;
}

void TouchFlow::Reset() {
    stage_ = TouchStage::kTest;
    collected_ = 0;
    outcome_ = TouchFit::kOk;
    result_at_ms_ = 0;
}

void TouchFlow::Start() {
    stage_ = TouchStage::kCollecting;
    collected_ = 0;
}

bool TouchFlow::Released(int16_t raw_x, int16_t raw_y, uint32_t held_ms) {
    if (stage_ != TouchStage::kCollecting || collected_ >= kTouchTargets) {
        return false;
    }
    if (held_ms < kMinPressMs || held_ms > kMaxPressMs) {
        return false;
    }
    samples_[collected_].raw_x = raw_x;
    samples_[collected_].raw_y = raw_y;
    ++collected_;
    return true;
}

TouchFit TouchFlow::Finish(uint16_t width, uint16_t height, TouchCalibration *out,
                           uint32_t now_ms) {
    // The crosses are filled in here rather than when the point arrived, so that
    // "where the operator was asked to press" comes from one function on both
    // sides of the exchange (`TouchTarget`) and cannot drift between them.
    for (uint8_t i = 0; i < collected_ && i < kTouchTargets; ++i) {
        TouchTarget(i, width, height, &samples_[i].target_x, &samples_[i].target_y);
    }

    outcome_ = FitTouch(samples_, collected_, width, height, out);
    stage_ = TouchStage::kResult;
    result_at_ms_ = now_ms;
    return outcome_;
}

bool TouchFlow::Tick(uint32_t now_ms) {
    if (stage_ != TouchStage::kResult) {
        return false;
    }
    if ((now_ms - result_at_ms_) < kResultMs) {
        return false;
    }
    Reset();
    return true;
}

}  // namespace ui
