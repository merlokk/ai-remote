#pragma once

// **The one output this device has** (CLAUDE.md §10.1, §10.17): a single WS2812
// on GPIO48, driven off UART1 with the encoding `led_frames.h` explains, and a
// task that keeps it showing whatever it was last told to show.
//
// The split either side of this file is the layering of §10.14.2 in miniature:
//
//   * `led_frames.h` is the arithmetic — colours, rhythms, the byte order. No
//     ESP-IDF in it, so §10.11's host tier runs all of it;
//   * this file is the peripheral and the task — a UART, a mutex and a sleep;
//   * `indicator.h` is the policy — which state of the device is which colour.
//     **Nothing in this file knows what an approval is**, and that is the test:
//     deleting `indicator` must leave a working LED.
//
// **Why a task at all.** Two of the six rhythms are time-varying and one of them
// has sixty steps; the alternative is every caller re-asserting the colour on a
// timer of its own, which is five timers where one will do. The task is cheap
// because `Animator::FrameAt` tells it when the next change is due — a solid
// colour wakes it once a second, a beacon twice every two seconds, and only the
// breath runs at a frame rate.

#include <cstdint>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_err.h"
#include "led_frames.h"

namespace led {

// Small: the task encodes twelve bytes and writes them. The measured high-water
// mark is printed by `led` on the console against this number, which is why it is
// a named constant rather than a literal at the `xTaskCreate`.
inline constexpr uint32_t kTaskStackBytes = 2560;

// **Low, and lower than the responder's** (which is 4). A frame that is one beat
// late is a light that flickers; a signature that is one beat late is a verdict
// that misses its reply subject. When the two contend, the verdict wins.
inline constexpr int kTaskPriority = 2;

// The longest the task will sleep with nothing changing. A ceiling rather than a
// period: it exists so a WS2812 that lost a bit to a glitch — a brown-out on the
// data line, a neighbour's inductive kick — is corrected within a second instead
// of staying wrong until the next state change, which on an idle device could be
// hours.
inline constexpr uint32_t kRefreshMs = 1000;

struct Status {
    bool ready = false;
    Rgb colour{};
    Effect effect = Effect::kSolid;
    uint8_t percent = 0;
    uint8_t idle_percent = 0;
    bool overriding = false;

    uint32_t writes = 0;
    uint32_t write_failures = 0;
    uint32_t stack_low_water = 0;
};

// Brings up the UART and starts the task. **The pin is an argument, not an
// include of `board.h`** (§10.14.2): this layer knows about a serial LED, not
// about which board it is soldered to.
//
// Safe to call before the filesystem: the brightness it starts at is the
// compiled-in default, and `SetBrightness` is what `main` calls once
// `config::Init` has said what the operator wanted.
esp_err_t Init(uart_port_t port, gpio_num_t pin);
bool Ready();

// The operator's two ceilings (`config::Led`). Applied to the next frame, so a
// `config reload` that lowers the brightness is visible without a state change.
void SetBrightness(uint8_t percent, uint8_t idle_percent);

// The state colour, at the full ceiling. This is what a request uses.
void Set(Rgb colour, Effect effect);

// The same, at the *idle* ceiling — for the resting states, which are most of
// the device's life. Two calls rather than a boolean argument because the call
// site then says which of the two it meant.
void SetIdle(Rgb colour, Effect effect);

// Show something for a moment and then go back to whatever was underneath.
// A verdict is the caller this exists for: the flash has to be seen, and what
// follows it is not this caller's business.
void SetFor(Rgb colour, Effect effect, uint32_t duration_ms);

// End a `SetFor` now instead of waiting it out. For the caller that put up a
// prompt and has just got its answer — see `Animator::EndFor`.
void EndFor();

void Off();

Status Get();

}  // namespace led
