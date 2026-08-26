#pragma once

// The fake ESP-IDF the host tests compile the real drivers against
// (CLAUDE.md §10.11).
//
// **The production code is not modified and does not know this exists.** The
// headers a driver includes are shadowed by fakes on the include path, so the
// file that ships is the file under test. That is the whole property: no
// `#ifdef _WIN32` in anything that goes on the board.
//
// **It is small because this board is** (§10.13): no I²C bus and no chip on one,
// no I²S, no display, no touch controller — so there is no peripheral here to
// model but the ones this firmware's host tier actually reaches. Two GPIOs, a
// clock a test can move, a non-blocking mutex, and the storage fake next door.

#include <cstddef>
#include <cstdint>

#include "driver/gpio.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

namespace fake {

struct Platform {
    static constexpr size_t kMaxPins = 40;

    // --- FreeRTOS ---
    // The mutex never blocks here, which is deliberate: a fake that blocked
    // could not tell "asked for 20 ms" from "asked for forever".
    bool mutex_taken;
    TickType_t last_take_ticks;
    size_t take_calls;
    size_t give_calls;

    // **Milliseconds slept**, which is what `test_buttons.cpp` asserts on: a
    // poll loop that waits the wrong amount of time still passes every level
    // check, and this is the only place the waiting itself is visible.
    uint32_t delay_ms_total;

    // **The clock, in microseconds, and it only moves when a test says so.**
    // `esp_timer_get_time()` reads it and `vTaskDelay` advances it — which is
    // not a convenience but a requirement: `Buttons::HeldFor` polls in a loop
    // with a delay between, so a clock that stood still would spin forever and
    // a clock that ran on its own would make "held for five seconds" a race.
    uint64_t clock_us;

    // --- GPIO ---
    int level[kMaxPins];
    // **Whether the world is holding this pin**, as opposed to an internal
    // pull deciding it. A button shorted to ground beats a pull-up, so
    // `gpio_config` must not overwrite a level a test has set — which is
    // exactly §10.15's case: the finger is on `BOOT` before `Init` runs.
    bool level_forced[kMaxPins];
    size_t rising_edges[kMaxPins];
    gpio_mode_t last_mode[kMaxPins];

    bool log_enabled;
};

Platform &P();

// Call it at the top of every test. `setUp` does, so a test that forgets is
// still clean.
void Reset();

// Moves the fake clock. `vTaskDelay` does the same thing, so a driver that
// waits does not need help from the test to get there.
void AdvanceMs(uint32_t ms);

// Sets what a pin reads, **without counting an edge**. Edges belong to what the
// firmware drives; this is the world putting a level on an input, which is a
// different thing and must not be mistaken for the first.
void SetPinLevel(gpio_num_t pin, int level);
size_t RisingEdges(gpio_num_t pin);

// Takes the mutex from outside, which is how a test plays "another task is
// holding it" without threads.
void TakeMutexFromAnotherTask();
void GiveMutexFromAnotherTask();

}  // namespace fake
