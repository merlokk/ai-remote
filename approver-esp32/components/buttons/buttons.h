#pragma once

// The board's three buttons, debounced (CLAUDE.md §10.1).
//
// There is no chip here — three GPIOs and a contact that bounces — so what this
// component owns is the part that is easy to get subtly wrong: a stable answer
// to "is it pressed", and how long it has been that way. §10.15 is the first
// caller with a real requirement: `KEY` held ≥ 5 s at boot restores the config,
// sampled before the filesystem and before the panel, so it must not depend on
// anything else being up.
//
// **Polled, not interrupt-driven, and no task of its own.** Nothing in this
// firmware needs a press faster than a poll delivers it, and an ISR plus a queue
// would be machinery with no consumer — the same argument §10.14.3 makes about
// the I²C fake. The owner polls (the LVGL task, at frame rate, when there is
// one) and gets edges back. When something needs a callback, it arrives with the
// thing that needs it.
//
// **The pins are arguments, not an include of `board.h`** (§10.14.2): this layer
// knows about contacts, not about which board they are soldered to.
// `components/boards` is what puts the two together, and it is also where the
// names `boot` / `key` / `pwr` live.
//
// A note that is hardware, not software: **`PWR` is not this firmware's
// button.** It is wired to the AXP2101's PWRON pin and the chip acts on it with
// no code running at all — §10.1 has the timings. Reading it here says what the
// operator's finger is doing; it does not make the board switch on or off.

#include <cstddef>
#include <cstdint>

#include "driver/gpio.h"
#include "esp_err.h"

namespace buttons {

// This board has three (§10.1). The extra slot is not ambition — it is so that
// a fourth wired button on a revision does not need this file edited to be
// tested from the console.
inline constexpr size_t kMaxButtons = 4;

// How long a level has to hold before it is believed. A contact settles in
// single-digit milliseconds; 25 ms is comfortably past that and still far below
// what a finger can notice.
inline constexpr uint32_t kDebounceMs = 25;

// The poll period the blocking helpers use, and the one a caller polling from
// its own loop should not go far above: an edge is seen at worst one period
// plus one debounce window late.
inline constexpr uint32_t kPollIntervalMs = 10;

enum class Event : uint8_t {
    kNone = 0,  // nothing changed since the last poll
    kPressed,   // a release→press edge, already debounced
    kReleased,  // a press→release edge
};

struct Config {
    gpio_num_t pin = GPIO_NUM_NC;
    const char *name = "";
    // Every button on this board shorts its pin to ground, so idle is high and
    // pressed is 0. Kept as a field rather than assumed, because the day it is
    // not true the wrong assumption reads as "the button is stuck down".
    bool active_low = true;
    // An internal pull-up guarantees a defined idle level whether or not the
    // board provides one of its own. Harmless where the board already pulls the
    // line up, which is the case for all three here.
    bool pull_up = true;
};

// The debounce, on its own and with no ESP-IDF in it: a level and a clock in,
// an edge out. Separated deliberately — it is the only part of this component
// with logic to get wrong, and it is the part §10.11's host tier can run
// without a board.
//
// Times are milliseconds in `uint32_t` and are only ever subtracted, so the
// wrap at ~49 days is not a special case to handle.
class Debounce {
   public:
    // Adopt a level without reporting an edge — what Init does, so that a
    // device booted with a finger already down does not see a press it missed.
    void Reset(bool pressed, uint32_t now_ms);

    Event Update(bool raw_pressed, uint32_t now_ms);

    bool Pressed() const { return pressed_; }

    // How long the current state — pressed *or* released — has held.
    uint32_t StableMs(uint32_t now_ms) const { return now_ms - changed_at_ms_; }

   private:
    bool pressed_ = false;
    bool candidate_ = false;
    uint32_t candidate_since_ms_ = 0;
    uint32_t changed_at_ms_ = 0;
};

class Buttons {
   public:
    Buttons() = default;
    Buttons(const Buttons &) = delete;
    Buttons &operator=(const Buttons &) = delete;

    // Trivial constructor, separate Init (§10.14.1). `configs` is copied into
    // this object — no heap, and the caller's table may be a temporary.
    esp_err_t Init(const Config *configs, size_t count);
    bool Ready() const { return count_ > 0; }
    size_t Count() const { return count_; }

    const char *Name(size_t index) const;
    gpio_num_t Gpio(size_t index) const;

    // The wire, right now, undebounced. For a console readout that wants to
    // show what the pin says rather than what the state machine believes.
    bool RawPressed(size_t index) const;

    // Sample one button and return the edge, if any. This is the call that
    // advances the state machine; everything below reads what it left behind.
    Event Poll(size_t index);

    // Sample all of them. Edges are dropped — for a caller that only wants
    // Pressed()/HeldMs() to be current.
    void PollAll();

    // The debounced state as of the last Poll of that button.
    bool Pressed(size_t index) const;

    // How long it has been in that state, as of now.
    uint32_t StableMs(size_t index) const;

    // Blocks until the button has been held for `hold_ms`, or returns false the
    // moment it is released — including immediately, when it was not down to
    // begin with. This is §10.15's boot check: five blind seconds
    // with no screen to give feedback on, which is the argument for a long
    // threshold rather than a short one. Costs one task delay per poll and
    // nothing else.
    bool HeldFor(size_t index, uint32_t hold_ms);

   private:
    bool Valid(size_t index) const { return index < count_; }

    Config configs_[kMaxButtons] = {};
    Debounce state_[kMaxButtons] = {};
    size_t count_ = 0;
};

}  // namespace buttons
