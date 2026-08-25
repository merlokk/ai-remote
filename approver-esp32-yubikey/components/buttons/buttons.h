#pragma once

// The board's one button, debounced (CLAUDE.md §10.1).
//
// There is no chip here — a GPIO and a contact that bounces — so what this
// component owns is the part that is easy to get subtly wrong: a stable answer
// to "is it pressed", and how long it has been that way. §10.15 is the first
// caller with a real requirement: `BOOT` held >= 5 s **after** boot restores the
// config, sampled before the settings file is parsed, so it must not depend on
// anything else being up.
//
// **This file is the sibling folder's, kept general on purpose.** That board has
// three buttons and this one has one; the class is a table of pins either way,
// and the two copies differ only in these opening paragraphs. The `PWR` note that
// used to be here is gone with the power-management chip it described — there is
// none on this board, and no I²C bus for one to sit on (§10.13).
//
// **Polled, not interrupt-driven, and no task of its own.** Nothing in this
// firmware needs a press faster than a poll delivers it, and an ISR plus a queue
// would be machinery with no consumer. The owner polls — on this board the
// responder's gate task, every
// `kPollIntervalMs` — and gets edges back. When something needs a callback, it
// arrives with the thing that needs it.
//
// **The pins are arguments, not an include of `board.h`** (§10.14.2): this layer
// knows about contacts, not about which board they are soldered to.
// `components/boards` is what puts the two together, and it is also where the
// name `BOOT` lives.
//
// A note that is hardware, not software: **`BOOT` is GPIO0, the ROM's download
// strap.** Held across a reset it means "flash me" and no code of ours runs at
// all. Everything this component does with it happens after boot, which is why
// §10.15's restore window opens where it does rather than at reset.

#include <cstddef>
#include <cstdint>

#include "driver/gpio.h"
#include "esp_err.h"

namespace buttons {

// This board has one (§10.1). The other three slots are not ambition — they are
// so that a second wired button on a revision, or a board this component is
// carried to, does not need this file edited to be tested from the console. They
// cost four bytes of `Config` each and nothing else.
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
    // The button on this board shorts its pin to ground, so idle is high and
    // pressed is 0. Kept as a field rather than assumed, because the day it is
    // not true the wrong assumption reads as "the button is stuck down".
    bool active_low = true;
    // An internal pull-up guarantees a defined idle level whether or not the
    // board provides one of its own. Harmless where the board already pulls the
    // line up, which GPIO0 does here — but a firmware that relies on a board's
    // pull-up reads a floating pin as a stuck button the day it is carried to a
    // revision that dropped it.
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

// A press, once it is over — or, for a long one, the moment it becomes long.
enum class Press : uint8_t {
    kNone = 0,
    kShort,  // released before the threshold
    kLong,   // held to the threshold, reported **while the finger is still down**
};

// Short press or long press, on a button somebody else is already polling
// (§10.15: `BOOT` held five seconds restores the config, and a tap on it denies
// a pending request). Fed the debounced level and a clock, like `Debounce` above
// and for the same reason — it is the only part of this with a rule in it, and
// §10.11 can run it with no board.
//
// **A long press fires at the threshold rather than at the release**, and that
// is the decision inside this class. The operator is holding a button with no
// feedback but one LED, so the LED has to change *while they are holding* — a
// device that waits for the finger to come up is a device somebody keeps
// holding, wondering. The release afterwards then reports nothing: one press is
// one action, and a long press that also delivered a short one on the way out
// would restore the config and then deny whatever request was pending.
class PressLength {
   public:
    explicit constexpr PressLength(uint32_t long_ms) : long_ms_(long_ms) {}

    // Call it every poll. Times are milliseconds and are only ever subtracted,
    // so the ~49-day wrap is arithmetic rather than a case (`Debounce` above).
    Press Update(bool pressed, uint32_t now_ms);

    // How long the current press has been going, 0 when there is none. For a
    // screen that wants to show the hold filling up rather than surprising
    // somebody with it.
    uint32_t HeldMs(uint32_t now_ms) const;

   private:
    uint32_t long_ms_;
    bool down_ = false;
    bool fired_ = false;  // this press has already been reported as long
    uint32_t down_at_ms_ = 0;
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
    // begin with. This is §10.15's restore check. Unlike the sibling board's,
    // this one is **not** blind: the LED is up before the window opens and goes
    // white solid for the length of it (§10.17), which is the one thing this
    // board has that the C6's five silent seconds did not. Costs one task delay
    // per poll and nothing else.
    bool HeldFor(size_t index, uint32_t hold_ms);

   private:
    bool Valid(size_t index) const { return index < count_; }

    Config configs_[kMaxButtons] = {};
    Debounce state_[kMaxButtons] = {};
    size_t count_ = 0;
};

}  // namespace buttons
