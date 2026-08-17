#pragma once

// What the clock screen shows, decided where it can be tested (CLAUDE.md
// §10.8.2, §10.11).
//
// **This file includes nothing but `<cstdint>`.** It is the sixth in this
// firmware to manage that — after `ui/navigator.h`, `wifi_policy.h`,
// `reachability.h`, `sync_policy.h` and `link_policy.h` — and it is here for
// the reason all five of those are: §10.11 makes the host tier the
// comprehensive one, and a clock face that reaches for LVGL is a clock face
// that can only be looked at. So the *decisions* live here — is the time
// trustworthy, how many signal bars, is the bus dot green, where on the glass
// the whole thing sits this minute — and painting them is `screens/`' job.
//
// The decisions it owns, each of them a sentence from §10.8 made executable:
//
//   * **an unset clock shows dashes.** §10.8.2: "A device that has never had
//     either shows `--:--`, not `00:00` — a plausible wrong time is worse than
//     an obviously unset one". The range this file believes is the RTC's own
//     (§10.8.2's 2024..2099), so the clock face and the sync agree about what
//     a believable year is;
//   * **the drift, because the panel is an AMOLED.** §10.8.1: "Anything
//     permanent (the clock, the dot) shifts by a few pixels on a slow timer".
//     Here that is a bounded Lissajous walk, and the two things worth pinning
//     about it are that it never leaves its box and that it never stops;
//   * **the water.** The digits are filled from a travelling interference of
//     two waves rather than a flat green — which is also why they are never at
//     full brightness: an unlit pixel costs an AMOLED nothing and a fully lit
//     one costs it lifetime, so the ramp below tops out well short of
//     `0x00FF00`;
//   * **three indicators, and each one has a state that is not a fault.** No
//     bus configured is not a red dot, no battery is not an empty battery, and
//     a radio that was switched off is not a radio that failed. A screen that
//     spells "nothing was asked of me" the same way as "I am broken" is a
//     screen nobody trusts twice.
//
// What it deliberately does not hold: a colour, a pixel, a font, or anything
// about approvals (§10.14.2). It is handed numbers and hands numbers back.

#include <cstdint>

namespace ui {

// --- the three indicators ------------------------------------------------

// What the radio is doing, in the four shapes the icon can take. Not
// `wifimgr::State`, which has six: the two access-point states of §10.9 are one
// glyph here, because "somebody asked for an AP" and "nothing would have us" are
// a distinction for a settings screen and not for a 40-pixel icon.
enum class WifiIcon : uint8_t {
    kOff,         // the radio is down because that is what was asked for
    kConnecting,  // an attempt is in flight, or a backoff is running
    kClient,      // joined, and `bars` says how well
    kAp,          // this device *is* the network
};

enum class BusIcon : uint8_t {
    kOff,     // no server configured — dim, and not a fault
    kDown,    // there is a server and we are not on it
    kUp,      // connected, and nothing has arrived lately
    kActive,  // connected, and something arrived inside `kTrafficWindowMs`
};

enum class BatteryIcon : uint8_t {
    kAbsent,       // no battery: the board is running off the cable
    kDischarging,  // on the battery, nothing feeding it
    kCharging,     // taking current
    kExternal,     // cable in, battery not taking current — full, or idle
};

struct ClockInputs {
    // The system clock, UTC seconds. **The model decides whether to believe
    // it**, because §10.8.2's dashes-rather-than-midnight rule is a decision
    // and belongs where a test can reach it.
    int64_t epoch_utc = 0;

    // The local wall clock, 24-hour. The zone is presentation and is applied by
    // the caller (§10.8.2: the clock is UTC and a zone changes only what is
    // printed), so nothing in here has ever seen one.
    uint8_t hour = 0;
    uint8_t minute = 0;

    WifiIcon wifi = WifiIcon::kOff;
    int8_t rssi = 0;

    bool bus_configured = false;
    bool bus_connected = false;

    // The delivery counter, monotonic for as long as one client object lives.
    // It goes **backwards** across a reconnect — the client is destroyed and
    // rebuilt (§10.5) — and that is a case with its own rule below.
    uint64_t bus_messages = 0;

    bool battery_present = false;
    bool vbus_present = false;
    bool charging = false;
    int16_t battery_percent = -1;  // negative when there is nothing to ask

    // `esp_timer` milliseconds. Only ever subtracted, so the ~49-day wrap is
    // arithmetic rather than a case to handle — the note `buttons.h`,
    // `wifi_policy.h` and `link_policy.h` all carry.
    uint32_t now_ms = 0;
};

struct ClockView {
    // Not a digit: the middle segment on its own, which is what a
    // seven-segment display has instead of a blank (§10.8.2's `--:--`).
    static constexpr int8_t kDash = -1;

    int8_t digit[4] = {kDash, kDash, kDash, kDash};
    bool time_valid = false;

    WifiIcon wifi = WifiIcon::kOff;
    uint8_t bars = 0;  // how many of `kMaxBars` are lit

    BusIcon bus = BusIcon::kOff;

    BatteryIcon battery = BatteryIcon::kAbsent;
    bool battery_known = false;  // is there a percentage worth printing
    uint8_t battery_percent = 0;

    // Where the whole face sits this instant, relative to its resting place.
    // The panel is the reason (§10.8.1); the renderer just moves one object.
    int16_t drift_x = 0;
    int16_t drift_y = 0;

    // Where the water is, 0..255 for one full cycle. The renderer turns it and
    // a pixel row into a colour through `Shimmer`.
    uint8_t phase = 0;
};

class ClockFace {
   public:
    // Mobile-style, and three rather than four because that is what §10.8.2's
    // icon has room for at this size.
    static constexpr uint8_t kMaxBars = 3;

    // "Something arrived recently" — two minutes, the same threshold §10.8.3
    // uses to call a `status` document stale, and kept equal to it on purpose.
    static constexpr uint32_t kTrafficWindowMs = 120000;

    // What years a clock is allowed to claim. The floor is 2024-01-01 and the
    // ceiling 2100-01-01 — **the RTC's own range** (§10.8.2: it stores two
    // digits and no century), so the face and `timesync` refuse the same
    // answers.
    static constexpr int64_t kEpochFloor = 1704067200;
    static constexpr int64_t kEpochCeiling = 4102444800;

    // --- the drift (§10.8.1) ---------------------------------------------
    // How far the face wanders from its resting place, in pixels each way. Big
    // enough that no pixel carries an edge for long, small enough that a clock
    // on a desk still reads as centred.
    static constexpr int16_t kDriftX = 30;
    static constexpr int16_t kDriftY = 40;

    // Two triangle waves at 3:5, which is what makes the path a lattice of
    // diagonals across the box rather than a line it retraces. It **does**
    // repeat, every `kCycleMs`, and that is the accepted cost of the next
    // paragraph: a path that repeats every sixteen minutes still moves every
    // edge over the whole box, and a path derived from an absolute millisecond
    // counter would jump once every ~49 days when that counter wrapped.
    static constexpr uint32_t kDriftPeriodXMs = 192000;
    static constexpr uint32_t kDriftPeriodYMs = 320000;

    // One water cycle. Slow enough to read as flowing rather than flickering.
    static constexpr uint32_t kPhasePeriodMs = 6400;

    // The cycle the model actually keeps, accumulated from elapsed time rather
    // than read off `now_ms`. Every period above divides it exactly, which is
    // what makes both the drift and the water continuous where it wraps —
    // asserted below rather than trusted.
    static constexpr uint32_t kCycleMs = 960000;

    // --- the water --------------------------------------------------------
    // The two wavelengths, in pixels, and they are deliberately not multiples
    // of each other: interference with a short period would read as a pattern.
    static constexpr int16_t kWaveLengthLong = 130;
    static constexpr int16_t kWaveLengthShort = 61;

    // The bottom of the ramp. **Not zero**: a trough at zero would break the
    // digits into floating bands, and the eye reads that as a fault rather than
    // as water. The top is 255 and the renderer is what decides that 255 is a
    // moderate green rather than a pure one (§10.8.1: static is expensive).
    static constexpr uint8_t kShimmerFloor = 96;

    static_assert(kCycleMs % kDriftPeriodXMs == 0, "the drift would jump");
    static_assert(kCycleMs % kDriftPeriodYMs == 0, "the drift would jump");
    static_assert(kCycleMs % kPhasePeriodMs == 0, "the water would jump");
    static_assert(kPhasePeriodMs % 256 == 0, "the phase step would not divide");

    ClockFace() = default;
    ClockFace(const ClockFace &) = delete;
    ClockFace &operator=(const ClockFace &) = delete;

    ClockView Update(const ClockInputs &in);

    // --- the arithmetic, exposed because it is worth pinning on its own ---

    // The drift at a point in the cycle. Static because it holds no state: the
    // walk is a function of where in the cycle we are, and the only thing the
    // instance above adds is keeping track of that.
    static void DriftAt(uint32_t cycle_ms, int16_t *dx, int16_t *dy);

    // How brightly the water lights a pixel row, 0..255. Two waves of
    // different wavelength travelling in opposite directions — one wave is a
    // wipe, and interference is what makes water look like water. The renderer
    // maps this onto a green; nothing here knows a colour.
    static uint8_t Shimmer(int16_t y, uint8_t phase);

    // One sine, 0..255, over 256 angle units — and exposed for one reason: the
    // table behind it has 64 entries, so **it interpolates**, and that is a
    // property worth an assertion of its own. Read flat, three angles in four
    // repeat their neighbour and the texture above gains a staircase two pixels
    // wide; the difference is too small to catch through `Shimmer`, which is how
    // the mutation pass found that this needed testing directly (§10.11).
    static uint8_t Wave(uint8_t angle);

    // Signal into bars, and **never zero**: a link that is up with a dreadful
    // signal is still up, and an icon showing nothing would read as an icon
    // showing "not connected".
    static uint8_t BarsFor(int8_t rssi);

   private:
    // Accumulated from the deltas between calls, and reduced mod `kCycleMs`.
    // Reading `now_ms % period` directly would be one line shorter and would
    // put a visible jump in the drift every time the millisecond counter
    // wrapped; a subtraction of two `uint32_t` has no such day.
    uint32_t cycle_ms_ = 0;
    uint32_t last_ms_ = 0;
    bool started_ = false;

    // The delivery counter as it was last seen, and when it last moved
    // **upwards**. The first reading establishes a baseline and is not traffic
    // — otherwise a device that has been up for an hour would light the dot the
    // moment the screen was created.
    uint64_t messages_ = 0;
    bool baselined_ = false;
    uint32_t traffic_ms_ = 0;
    bool traffic_ = false;
};

}  // namespace ui
