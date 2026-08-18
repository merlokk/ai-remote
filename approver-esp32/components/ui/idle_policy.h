#pragma once

// When the panel dims, and when it goes off (CLAUDE.md §10.8.1).
//
// **This file includes `<cstdint>` and nothing else**, so all of it runs under
// Unity with no board (§10.11) — the twelfth subject in this firmware to manage
// that, after the navigator, the Wi-Fi policy, the internet check, the sync
// schedule, the bus link, the clock face, the request card, the limits, the
// settings list and the touch correction. The panel commands are
// `screens/screens.cpp`'s and the pixels are nobody's: this decides a
// brightness, not a layout.
//
// ## Why this exists at all
//
// §10.8.1 states the rule and then nothing kept it: "brightness drops on an idle
// timeout and the panel blanks after it — waking on touch, and unconditionally
// on a request. Burn-in on a device showing one layout for months is an outcome,
// not a risk." The two settings that were supposed to carry it — `dimSeconds`
// and `blankSeconds` — were parsed, saved, printed, round-tripped through
// `config save`, covered by three host tests, and **read by nothing at all**.
// A setting that survives a reboot and changes nothing is worse than a missing
// one, which is the same finding §10.15 records about the brightness, and this
// is that finding a second time on the two fields next to it.
//
// So both are gone, under new names, so that a `config.json` already on a device
// cannot bring a 30-second dim with it into firmware where 30 seconds means
// something else.
//
// ## The two thresholds, and why only one of them needs the IMU
//
// **Dimming is unconditional.** Nobody has ever been annoyed by a clock that got
// quieter, and an AMOLED showing one layout at full brightness is the burn-in
// §10.8.1 calls an outcome.
//
// **Going off is not**, and the condition is the repository owner's: only when
// the board is standing on its USB edge with the button edge up. The reasoning
// is that this device's whole value is a glance at the desk telling you the loop
// is alive (§10.8.2), and a black square tells you nothing — so the panel is
// only allowed to go dark in the one position nobody reads a clock in, which on
// this desk means the board stood up out of the way. Lying flat it dims and
// stops there, however long it is left.
//
// The sign of that axis is the part worth being careful about: §10.13 measured
// every one of them by putting the board in a known position and reading it, and
// the reading for *buttons up* is **+1 g on Y** — gravity acts along −Y, and an
// accelerometer at rest reads the support force rather than the pull. Getting it
// backwards is invisible on a desk and exactly wrong the moment the thing is
// stood up.
//
// ## What counts as something happening
//
// Everything: a press, a finger, the board being moved, a `status` document, a
// request card, a notice under the clock. The caller decides what to feed in —
// this holds one timestamp — and the reason that is a design and not laziness is
// §10.10: a device that dimmed while a request was on the glass would be a
// device making the one screen it exists for harder to read.
//
// `Activity` is deliberately the cheapest possible call — one store — because it
// is made from tasks that are not the screen task: the bus task raises a card,
// and the watcher hands over a document. Nothing is recomputed there; `Tick`
// runs on the screen task and is the only thing that changes state.

#include <cstdint>

namespace ui {

// What the panel should be doing. Three states rather than a brightness and a
// flag, because "off" is not "0 %": a display at zero is still refreshing, and
// on an AMOLED the difference is the entire point (`panel.h` says the same
// thing from the driver's side).
enum class DisplayPower : uint8_t {
    kFull,  // the brightness `config.json` asked for
    kDim,   // the idle level, also from the file
    kOff,   // the panel is off, and only ever standing on its USB edge
};

const char *DisplayPowerName(DisplayPower state);

struct IdleSettings {
    // 0 disables, for both — and 0 is a real answer rather than a missing one,
    // the same call §10.8.2 makes about `syncHours: 0`. A device on a bench that
    // must never change what it is showing is a device with both at zero.
    uint32_t dim_after_ms = 0;
    uint32_t sleep_after_ms = 0;

    uint8_t full_percent = 100;
    uint8_t dim_percent = 30;
};

// **Which way up the board is** — §10.13's table, and the one place in this
// firmware that turns three accelerometer numbers into a position. Every row of
// that table was established by putting the board in that position and reading
// it, never derived from a drawing, which is the only way a sign is ever right.
//
// It has two readers, and that is the point of it living here: the blank of
// §10.8.1 asks whether this is `kUsbEdge`, and the motion status page of §10.8.5
// prints the name. A screen saying the board is standing while the panel refused
// to go dark would be a disagreement nobody could diagnose from the outside.
enum class Orientation : uint8_t {
    // Nothing dominant: on a corner, in a hand, or being carried. §10.9's rule
    // that `unknown` is the honest state, arriving on a different subject — and
    // the blank needs a statement rather than the absence of one.
    kUnknown = 0,
    kScreenUp,     // flat on the desk, which is how it spends its life
    kScreenDown,
    kUsbEdge,      // standing on the connector, buttons up: the one that blanks
    kButtonEdge,
    kSpeakerEdge,
    kCardSlotEdge,
};

Orientation OrientationOf(float x, float y, float z);

// For a screen. Short on purpose: the status page's value column holds 21
// characters, counted against the font rather than guessed, and a host test pins
// that here because this layer cannot see the constant.
const char *OrientationName(Orientation orientation);

// The axis gravity acts along, for the console, which prints
// `gravity along +X (standing on the card-slot edge)`. Empty for `kUnknown`,
// which is what lets that one caller take its other branch rather than print an
// empty pair of parentheses.
const char *OrientationAxis(Orientation orientation);

// **Is the board standing on its USB edge, buttons up** — the question the blank
// asks, and a reading of the table above rather than a second opinion about it.
// Free rather than a member because it is the IMU's answer and not the policy's
// state, which keeps `screens.cpp` from having to know which axis is which.
bool StandingButtonsUp(float x, float y, float z);

// Two accelerometer samples far enough apart to call it movement. Measured on
// this board rather than assumed: at rest the magnitude wanders about 0.02 g and
// the gyro sits at −3 dps, so a threshold under that is a device that never
// dims. It compares the **whole vector**, because a rotation about gravity moves
// three axes a little and none of them much.
bool Moved(const float previous[3], const float current[3]);

// The one number above, exposed so the test and the code state it once.
inline constexpr float kMotionThresholdG = 0.08f;

// And the one below, for `StandingButtonsUp`: how much of the vector has to be
// on one axis before that axis names a position. The same 0.7 g the console's
// `GravityAxis` uses, and for the same reason — a stand holds the board leaning
// back, so the test cannot demand a clean 1.0.
inline constexpr float kDominantG = 0.7f;

class IdlePolicy {
   public:
    // Replaces the settings and **nothing else** — the idle clock keeps running.
    // That is what makes `config set dim 60` apply to a screen that has already
    // been idle ten minutes instead of starting the wait again, and it is why
    // typing on the console is not activity: the console is not the glass.
    void Configure(const IdleSettings &settings);
    const IdleSettings &Settings() const { return settings_; }

    // Something happened. One store, safe to call from any task (§10.8.1 keeps
    // every decision on the screen task, and this is not one).
    void Activity(uint32_t now_ms) { since_ms_ = now_ms; }

    void SetUpright(bool upright) { upright_ = upright; }
    bool Upright() const { return upright_; }

    // True when the state changed and the panel has to be told. The caller sends
    // a QSPI command on every `true`, which is why this is an edge and not a
    // level: the panel shares those wires with the frame being drawn on it.
    bool Tick(uint32_t now_ms);

    DisplayPower State() const { return state_; }

    // What the panel should be at now. Never brighter than the configured full
    // level — a hand-edited file can say `brightness 20, dimPercent 60`, and a
    // screen that brightened when it gave up waiting is a setting nobody could
    // explain.
    uint8_t Brightness() const;

    uint32_t IdleMs(uint32_t now_ms) const { return now_ms - since_ms_; }

   private:
    DisplayPower Wanted(uint32_t now_ms) const;

    IdleSettings settings_ = {};
    uint32_t since_ms_ = 0;
    bool upright_ = false;
    DisplayPower state_ = DisplayPower::kFull;
};

}  // namespace ui
