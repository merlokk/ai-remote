#pragma once

// The screens of CLAUDE.md §10.8, and the one task that keeps them current.
//
// It is `wifimgr` / `nats` / `timesync` one layer up and in the same shape: a
// task, a snapshot of the world taken at one instant, and no decisions of its
// own — the decisions are `ui::ClockFace`'s (host-tested, §10.11) and the pixels
// are `clock_screen.cpp`'s. What is left in the middle is *gathering*, which is
// the part that cannot be tested without a board and is therefore deliberately
// the part with nothing in it worth testing.
//
// One of the five screens exists (§10.8.2, the clock). When the other four
// arrive, this is where the navigator (`ui/navigator.h`) will sit: which screen
// is up is its answer, and this task is what would carry that answer to LVGL.
//
// Two rules it exists to keep, both §10.8.1's:
//
//   * **the LVGL task owns the display.** Nothing here touches a widget outside
//     `display::Lock`, and the lock is taken with a bound — an LVGL lock waited
//     on forever from another task is a hang that looks like a hardware fault;
//   * **an I²C read never happens inside an LVGL callback.** The PMIC is a dozen
//     registers under a lease (§10.14.3) and this task is where that happens, at
//     its own slow rate, well away from the frame the operator is looking at.
//
// The PMIC is passed in rather than reached for, the way `timesync::Init` takes
// the RTC: this component has never heard of `board.h`, and `main` is where the
// two meet (§10.14.2).

#include <cstdint>

#include "axp2101.h"
#include "clock_face.h"
#include "esp_err.h"

namespace screens {

// How often the world is re-read and the water moved. Ten frames a second is
// what makes a travelling gradient read as flowing rather than as stepping, and
// it is affordable because only the digits are repainted at that rate.
inline constexpr uint32_t kTickMs = 100;

// Below LVGL's own task (`display::kLvglTaskPriority`), because this one's job
// is to hand it work rather than to compete with it.
inline constexpr int kTaskPriority = 3;
inline constexpr uint32_t kTaskStackBytes = 4096;

// How long to wait for the LVGL lock before giving this tick up. A skipped
// frame is invisible; a blocked task is a watchdog panic with somebody else's
// name on it.
inline constexpr uint32_t kLockTimeoutMs = 100;

// The battery is read every this-many ticks rather than every tick: it is a
// dozen I²C registers under a lease, and a charge percentage that is two seconds
// stale is a charge percentage.
inline constexpr uint32_t kBatteryEveryTicks = 20;

// What is on the glass, taken at one instant — what `clock` on the console
// prints. **A snapshot rather than a look at LVGL**: the console must not touch a
// widget (§10.8.1's one-task rule) and must not wait behind the display lock to
// answer a question, so the task keeps this up to date and nothing here reaches
// into anything.
//
// It exists because a screen is the one part of this firmware whose output cannot
// be captured from a script. Every other component has a readout the four-places
// rule of §10.7 hangs off; without this one, "the drift is moving" would be a
// claim nobody could check without a camera.
struct Status {
    bool ready = false;

    ui::ClockView view = {};

    uint32_t updates = 0;      // how many times the face has been recomputed
    uint32_t lock_misses = 0;  // …and how many of those gave the frame up

    // The **lowest** free stack the task has ever had, the same call §10.14.1
    // makes about the heap: the minimum ever seen is the number that says
    // whether the device is safe.
    uint32_t stack_low_water = 0;
};

// Builds the screens on LVGL's active screen and starts the task. LVGL has to be
// up already — `main` starts it — and a null `battery` is allowed: the icon then
// says what it always says when there is nothing to ask.
esp_err_t Init(pmic::Axp2101 *battery);
bool Ready();

Status Get();

}  // namespace screens
