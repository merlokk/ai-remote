#include "idle_policy.h"

namespace ui {

namespace {

// Absolute value without `<cmath>`, which this file deliberately does not
// include: `<cstdint>` and nothing else is what keeps the whole of it runnable
// under Unity (§10.11), and one comparison is cheaper than the dependency.
constexpr float Abs(float v) { return v < 0.0f ? -v : v; }

}  // namespace

const char *DisplayPowerName(DisplayPower state) {
    switch (state) {
        case DisplayPower::kFull:
            return "lit";
        case DisplayPower::kDim:
            return "dimmed";
        case DisplayPower::kOff:
            return "off";
    }
    return "?";
}

Orientation OrientationOf(float x, float y, float z) {
    const float ax = Abs(x);
    const float ay = Abs(y);
    const float az = Abs(z);

    // One axis has to carry enough of the vector to name a position at all.
    // Below the threshold the board is on a corner or in somebody's hand, and
    // neither is a reason to switch the panel off or to claim a position on a
    // screen.
    //
    // **The sign is the half §10.13 says to measure rather than derive.** An
    // accelerometer at rest reads +1 g along the axis pointing *up*, because it
    // measures the support force and not the pull — so a positive reading names
    // the edge that is up, and the board is standing on the opposite one.
    if (az >= ax && az >= ay && az >= kDominantG) {
        return z > 0.0f ? Orientation::kScreenDown : Orientation::kScreenUp;
    }
    if (ay >= ax && ay >= az && ay >= kDominantG) {
        return y > 0.0f ? Orientation::kUsbEdge : Orientation::kButtonEdge;
    }
    if (ax >= ay && ax >= az && ax >= kDominantG) {
        return x > 0.0f ? Orientation::kSpeakerEdge : Orientation::kCardSlotEdge;
    }
    return Orientation::kUnknown;
}

const char *OrientationName(Orientation orientation) {
    switch (orientation) {
        case Orientation::kScreenUp:
            return "flat, screen up";
        case Orientation::kScreenDown:
            return "flat, screen down";
        case Orientation::kUsbEdge:
            return "USB edge down";
        case Orientation::kButtonEdge:
            return "button edge down";
        case Orientation::kSpeakerEdge:
            return "speaker edge down";
        case Orientation::kCardSlotEdge:
            return "card-slot edge down";
        case Orientation::kUnknown:
            break;
    }
    return "tilted or moving";
}

const char *OrientationAxis(Orientation orientation) {
    switch (orientation) {
        case Orientation::kScreenUp:
            return "+Z";
        case Orientation::kScreenDown:
            return "-Z";
        case Orientation::kUsbEdge:
            return "-Y";
        case Orientation::kButtonEdge:
            return "+Y";
        case Orientation::kSpeakerEdge:
            return "-X";
        case Orientation::kCardSlotEdge:
            return "+X";
        case Orientation::kUnknown:
            break;
    }
    // Empty rather than a placeholder, so the console can tell there is no axis
    // to print instead of printing one that means nothing.
    return "";
}

bool StandingButtonsUp(float x, float y, float z) {
    return OrientationOf(x, y, z) == Orientation::kUsbEdge;
}

bool Moved(const float previous[3], const float current[3]) {
    // The whole vector rather than an axis at a time. A rotation about gravity
    // changes three components a little and none of them much, and three
    // separate comparisons against the same threshold would miss it.
    float sum = 0.0f;
    for (int i = 0; i < 3; ++i) {
        const float d = current[i] - previous[i];
        sum += d * d;
    }
    return sum > kMotionThresholdG * kMotionThresholdG;
}

void IdlePolicy::Configure(const IdleSettings &settings) { settings_ = settings; }

uint8_t IdlePolicy::Brightness() const {
    const uint8_t dim =
        settings_.dim_percent < settings_.full_percent ? settings_.dim_percent : settings_.full_percent;
    switch (state_) {
        case DisplayPower::kFull:
            return settings_.full_percent;
        case DisplayPower::kDim:
        case DisplayPower::kOff:
            // The panel is off in the third case, so the number is what it comes
            // back to if something switches it on without going through here —
            // the console's `display on`, for one.
            return dim;
    }
    return settings_.full_percent;
}

DisplayPower IdlePolicy::Wanted(uint32_t now_ms) const {
    // Wrap-safe by subtraction rather than by comparing deadlines: the elapsed
    // time is what wraps correctly, and reaching ~49 days of it needs a device
    // nobody has touched for ~49 days, which is precisely the device this file
    // is about.
    const uint32_t idle = now_ms - since_ms_;

    // **Off outranks dim**, so a hand-edited file with a sleep shorter than its
    // dim still switches the panel off — the stronger of the two statements is
    // the one that wins rather than the one that happens to be checked first.
    if (settings_.sleep_after_ms != 0 && upright_ && idle >= settings_.sleep_after_ms) {
        return DisplayPower::kOff;
    }
    if (settings_.dim_after_ms != 0 && idle >= settings_.dim_after_ms) {
        return DisplayPower::kDim;
    }
    return DisplayPower::kFull;
}

bool IdlePolicy::Tick(uint32_t now_ms) {
    const DisplayPower wanted = Wanted(now_ms);
    if (wanted == state_) {
        return false;
    }
    state_ = wanted;
    return true;
}

}  // namespace ui
