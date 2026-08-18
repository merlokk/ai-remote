#include "clock_face.h"

namespace ui {
namespace {

// One period of a sine, 0..255, at 64 points — the whole of the floating-point
// mathematics this firmware would otherwise want, precomputed. `LV_USE_FLOAT` is
// off and the C6 has no FPU, so a `sinf` here would be a soft-float call per
// pixel row per frame.
constexpr uint8_t kSine[64] = {
    128, 140, 152, 165, 176, 188, 198, 208, 218, 226, 234, 240, 245, 250, 253, 254,
    255, 254, 253, 250, 245, 240, 234, 226, 218, 208, 198, 188, 176, 165, 152, 140,
    128, 115, 103, 90,  79,  67,  57,  47,  37,  29,  21,  15,  10,  5,   2,   1,
    0,   1,   2,   5,   10,  15,  21,  29,  37,  47,  57,  67,  79,  90,  103, 115,
};

// A triangle from -amp at t=0, through +amp at the half period, and back. A
// triangle rather than the sine above on purpose: a sine lingers at the
// extremes, which is exactly where a burn-in drift should not linger.
int16_t Triangle(uint32_t t, uint32_t period, int16_t amp) {
    const int32_t half = static_cast<int32_t>(period / 2);
    const int32_t at = static_cast<int32_t>(t);
    const int32_t a = amp;
    if (at < half) {
        return static_cast<int16_t>(-a + (2 * a * at) / half);
    }
    return static_cast<int16_t>(a - (2 * a * (at - half)) / half);
}

}  // namespace

// The table has 64 entries and the angle has 256 values, so four angles share a
// pair — **interpolated rather than repeated**, because reading the table flat
// puts a step every four angle units, and a step in a texture is a stripe with an
// edge on it. Public because the difference it makes is too small to see through
// `Shimmer`, so it is asserted here instead (`clock_face.h` says so).
uint8_t ClockFace::Wave(uint8_t angle) {
    const uint8_t index = static_cast<uint8_t>(angle >> 2);
    const int32_t here = kSine[index];
    const int32_t next = kSine[(index + 1) & 63];
    const int32_t frac = angle & 0x03;
    return static_cast<uint8_t>(here + ((next - here) * frac) / 4);
}

uint8_t ClockFace::BarsFor(int8_t rssi) {
    // The thresholds a phone uses, roughly: a good link, a usable one, and one
    // that works. **The floor is one bar and not none** — see the header.
    if (rssi >= -60) {
        return 3;
    }
    if (rssi >= -72) {
        return 2;
    }
    return 1;
}

uint8_t ClockFace::Shimmer(int16_t y, uint8_t phase) {
    // Two travelling waves, in opposite directions and at different speeds. The
    // long one carries the shape, the short one moving the other way is what
    // stops the result reading as a wipe.
    const int32_t row = y;
    const uint8_t down =
        static_cast<uint8_t>(row * 256 / kWaveLengthLong - static_cast<int32_t>(phase));
    const uint8_t up =
        static_cast<uint8_t>(row * 256 / kWaveLengthShort + static_cast<int32_t>(phase) / 2);

    // Weighted, so the long wave is the one the eye follows.
    const int32_t mix = (2 * static_cast<int32_t>(Wave(down)) + Wave(up)) / 3;

    // Into [kShimmerFloor, 255]. The floor is the reason this is a scale and not
    // the mix itself.
    const int32_t span = 255 - kShimmerFloor;
    return static_cast<uint8_t>(kShimmerFloor + (mix * span) / 255);
}

void ClockFace::DriftAt(uint32_t cycle_ms, int16_t *dx, int16_t *dy) {
    *dx = Triangle(cycle_ms % kDriftPeriodXMs, kDriftPeriodXMs, kDriftX);
    *dy = Triangle(cycle_ms % kDriftPeriodYMs, kDriftPeriodYMs, kDriftY);
}

ClockView ClockFace::Update(const ClockInputs &in) {
    ClockView view;

    // --- where in the cycle we are ---------------------------------------
    // Advanced by the elapsed time rather than read off `now_ms`: see the
    // header. The reduction before the addition is not tidiness — a task
    // starved for longer than one cycle would otherwise overflow the sum.
    if (!started_) {
        started_ = true;
        last_ms_ = in.now_ms;
    }
    const uint32_t delta = in.now_ms - last_ms_;  // wrap-safe: unsigned
    last_ms_ = in.now_ms;
    cycle_ms_ = (cycle_ms_ + (delta % kCycleMs)) % kCycleMs;

    DriftAt(cycle_ms_, &view.drift_x, &view.drift_y);
    view.phase = static_cast<uint8_t>(cycle_ms_ / (kPhasePeriodMs / 256));

    // --- the time --------------------------------------------------------
    // Two separate questions, and both have to answer yes: is the epoch a year
    // this device could be in, and is the wall clock a time at all. The second
    // is not paranoia about `localtime` so much as the refusal to render
    // `digit[0] = 7` out of an hour of 74.
    const bool believable = in.epoch_utc >= kEpochFloor && in.epoch_utc < kEpochCeiling;
    const bool on_the_clock = in.hour < 24 && in.minute < 60;
    view.time_valid = believable && on_the_clock;
    if (view.time_valid) {
        view.digit[0] = static_cast<int8_t>(in.hour / 10);
        view.digit[1] = static_cast<int8_t>(in.hour % 10);
        view.digit[2] = static_cast<int8_t>(in.minute / 10);
        view.digit[3] = static_cast<int8_t>(in.minute % 10);
    }
    // else the four dashes it was constructed with (§10.8.2).

    // --- the radio -------------------------------------------------------
    view.wifi = in.wifi;
    switch (in.wifi) {
        case WifiIcon::kClient:
            view.bars = BarsFor(in.rssi);
            break;
        case WifiIcon::kConnecting:
            // Never zero, so it cannot be mistaken for the radio being off.
            // Off the cycle rather than off the water's phase: an attempt is
            // worth showing at its own pace, and 400 ms reads as trying.
            view.bars = static_cast<uint8_t>(1 + (cycle_ms_ / 400) % kMaxBars);
            break;
        case WifiIcon::kOff:
        case WifiIcon::kAp:
            // Both draw the hollow shape; the access point adds its own mark.
            view.bars = 0;
            break;
    }

    // --- the bus ---------------------------------------------------------
    // The counter, before the state: what it did has to be recorded even on the
    // passes where the dot is not going to show it.
    if (!baselined_) {
        baselined_ = true;
        messages_ = in.bus_messages;
    } else if (in.bus_messages > messages_) {
        traffic_ = true;
        traffic_ms_ = in.now_ms;
    }
    // A counter that went *down* is a reconnect — the client object is rebuilt
    // and starts at zero (§10.5) — so it re-baselines and claims nothing. The
    // assignment covers that case and the ordinary one at once.
    messages_ = in.bus_messages;

    const bool recent = traffic_ && (in.now_ms - traffic_ms_) < kTrafficWindowMs;
    if (!in.bus_configured) {
        view.bus = BusIcon::kOff;
    } else if (!in.bus_connected) {
        view.bus = BusIcon::kDown;
    } else if (recent) {
        view.bus = BusIcon::kActive;
    } else {
        view.bus = BusIcon::kUp;
    }

    // --- the battery -----------------------------------------------------
    if (!in.battery_present) {
        view.battery = BatteryIcon::kAbsent;
    } else if (in.charging) {
        view.battery = BatteryIcon::kCharging;
    } else if (in.vbus_present) {
        view.battery = BatteryIcon::kExternal;
    } else {
        view.battery = BatteryIcon::kDischarging;
    }

    // --- the notice ------------------------------------------------------
    // Subtraction rather than a comparison of two absolute counters, so the
    // ~49-day wrap is arithmetic and not a case (the header, and every other
    // window in this firmware).
    view.notice = in.notice && (in.now_ms - in.notice_since_ms) < kNoticeMs;

    // A negative percentage is "there is nothing to ask" and must not read as an
    // empty battery — the same call §10.8.2 makes about an unset clock.
    view.battery_known = in.battery_present && in.battery_percent >= 0;
    if (view.battery_known) {
        const int16_t percent = in.battery_percent > 100 ? 100 : in.battery_percent;
        view.battery_percent = static_cast<uint8_t>(percent);
    }

    return view;
}

}  // namespace ui
