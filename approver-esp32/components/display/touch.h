#pragma once

// The touch panel — CST9220, on the shared I²C bus (CLAUDE.md §10.1, §10.14.3).
//
// Library layer, same as `panel.h`: it reports where a finger is, and has no
// idea that a press might become a verdict (§10.14.2).
//
// **The driver is `waveshare/esp_lcd_touch_cst9217`.** §10.4 says CST9220,
// which is the part number in the board's documentation; CST9217 is the driver
// Waveshare publishes for that board and uses in its own example. Same family,
// same register interface.
//
// **This is the file where §10.14.3 nearly got lost, so the reason it did not
// is written here.** `esp_lcd_touch` opens its own I²C device on the bus handle
// and talks to it directly — it knows nothing about our lease, and it cannot be
// taught. Two ways out were available:
//
//   * hand the touch handle to `lvgl_port_add_touch` and let the port poll it.
//     That is the short path, and it puts an I²C transfer in the LVGL task that
//     no lease covers: a read-modify-write on the PMIC could be split by a
//     touch read, which is the exact failure §10.14.3 exists to prevent;
//   * poll it ourselves, holding the lease across the call. Twenty lines, and
//     the invariant survives.
//
// The second one is what this is. `Read` takes the lease, calls the driver, and
// releases — so the vendor's transfers happen inside our critical section even
// though the vendor has never heard of it. A failed acquire is a dropped
// frame's read, never a block: §10.14.3 names touch as the case where that is
// the right answer.

#include <cstdint>

#include "esp_err.h"
#include "esp_lcd_touch.h"
#include "hal/gpio_types.h"
#include "i2c_bus.h"

namespace display {

struct TouchConfig {
    gpio_num_t reset = GPIO_NUM_NC;
    gpio_num_t interrupt = GPIO_NUM_NC;

    int width = 480;
    int height = 480;

    // The orientation this glass is mounted at, from the vendor's example. It
    // is not derivable from anything: a controller reports its own axes, and
    // which way round they are is how the film was laid down.
    bool swap_xy = true;
    bool mirror_x = false;
    bool mirror_y = true;
};

class Touch {
   public:
    Touch() = default;
    Touch(const Touch &) = delete;
    Touch &operator=(const Touch &) = delete;

    esp_err_t Init(i2cbus::Bus &bus, const TouchConfig &config);
    bool Ready() const { return handle_ != nullptr; }

    // True while a finger is down, with the coordinates written out. Takes the
    // I²C lease for the length of the read and gives it straight back; a lease
    // it could not get is reported as "no touch", which is the same thing a
    // dropped frame's read looks like from LVGL.
    bool Read(uint16_t *x, uint16_t *y);

    // How many reads were skipped because the bus was busy. Not a statistic for
    // its own sake: a number that climbs is contention worth looking at, and
    // one that stays at zero is the lease costing nothing.
    uint32_t MissedReads() const { return missed_; }

   private:
    i2cbus::Bus *bus_ = nullptr;
    esp_lcd_touch_handle_t handle_ = nullptr;
    uint32_t missed_ = 0;
};

}  // namespace display
