#pragma once

// PCF85063A — the board's real-time clock (CLAUDE.md §10.1), backed by the PMIC
// so it keeps time across a power cut. §10.8.2 gives it its job: it is the time
// source at boot, instantly and offline, and SNTP corrects it later.
//
// **Where the register map comes from.** `docs/Pcf85063atl1118-*.pdf`, NXP's own
// datasheet, rev 6 — Table 5 (register overview), Table 6 (Control_1) and
// Table 19 (Seconds). Not from memory, and not from the vendor's managed
// component: `waveshare/pcf85063a` would be a new dependency under root §1, and
// it takes a raw bus handle, which would walk straight past the lease of
// §10.14.3.
//
// Library layer (§10.14.2): it reads a chip, and knows nothing about approvals.

#include <cstdint>

#include "esp_err.h"
#include "i2c_bus.h"

namespace rtc {

// Slave address 1010001b. The datasheet also quotes A2h/A3h — those are the
// same address with the R/W bit already in place.
inline constexpr uint8_t kAddress = 0x51;

struct DateTime {
    uint16_t year;    // full year; the chip stores 00..99 and this driver adds 2000
    uint8_t month;    // 1..12
    uint8_t day;      // 1..31
    uint8_t weekday;  // 0..6, kept because the chip has it; nothing reads it yet
    uint8_t hour;     // 0..23 — this driver never leaves 24-hour mode
    uint8_t minute;   // 0..59
    uint8_t second;   // 0..59
};

class Pcf85063 {
   public:
    Pcf85063() = default;
    Pcf85063(const Pcf85063 &) = delete;
    Pcf85063 &operator=(const Pcf85063 &) = delete;

    // This chip has no identity register, so "present" means it acknowledged
    // its address. Trivial constructor, separate Init (§10.14.1).
    esp_err_t Init(i2cbus::Bus &bus);
    bool Present() const { return present_; }

    // Reads the seven counters in **one** burst from 04h. The datasheet is
    // explicit that a read freezes the counters, so a burst cannot catch a
    // carry halfway; two separate reads can, and would hand back the minutes
    // from one moment and the hours from the next.
    //
    // `*valid` is false when the chip's OS flag is set — the oscillator has
    // stopped or has never started, so the value is not to be believed. That is
    // exactly the state §10.8.2 wants shown as `--:--` rather than as a
    // plausible wrong time. It is also false when a field is out of range.
    esp_err_t Read(DateTime *out, bool *valid);

    // Writes all seven counters in one access, with the clock stopped around
    // it — the same carry problem in the other direction. Writing seconds
    // clears the OS flag, so a successful Write is what makes the clock
    // trustworthy again.
    esp_err_t Write(const DateTime &value);

   private:
    i2cbus::Bus *bus_ = nullptr;
    bool present_ = false;
};

}  // namespace rtc
