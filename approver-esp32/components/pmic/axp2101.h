#pragma once

// AXP2101 — the board's power management and charging chip (CLAUDE.md §10.1).
// §10.13 gives it one job: report charge. It is also, per §10.1, the thing that
// holds the panel's reset (ALDO3) and the amplifier's enable (ALDO2), so it is
// the first chip on the bus that has to work.
//
// **Where the register map comes from.** Not from memory, and not from this
// firmware's author: Waveshare ships `XPowersLib` in the examples repository
// for this board, and every address, bit and conversion below is read off
// `src/REG/AXP2101Constants.h` and `src/XPowersAXP2101.tpp` there — the same
// rule §10.1 states for pins. `docs/AXP2101_Datasheet_V1.4.pdf` is the paper
// behind it.
//
// Library layer (§10.14.2): it reads a chip, and knows nothing about approvals.

#include <cstdint>

#include "esp_err.h"
#include "i2c_bus.h"

namespace pmic {

inline constexpr uint8_t kAddress = 0x34;
inline constexpr uint8_t kChipId = 0x4A;

// The low three bits of STATUS2. The raw code is kept in `Status` next to the
// name so a wrong label is visible rather than believed.
enum class ChargeState : uint8_t {
    kTrickle = 0,
    kPreCharge = 1,
    kConstantCurrent = 2,
    kConstantVoltage = 3,
    kDone = 4,
    kNotCharging = 5,
};

struct Status {
    bool battery_present;
    bool vbus_present;
    bool charging;   // STATUS2 bits 6:5 == 01
    bool discharging;  // == 10
    uint8_t charge_code;  // STATUS2 & 0x07, as read
    uint16_t battery_mv;
    uint16_t vbus_mv;
    uint16_t system_mv;
    float die_celsius;
    int battery_percent;  // -1 when there is no battery to ask about
};

class Axp2101 {
   public:
    Axp2101() = default;
    Axp2101(const Axp2101 &) = delete;
    Axp2101 &operator=(const Axp2101 &) = delete;

    // Identifies the chip and turns on the ADC channels this firmware reads.
    // Trivial constructor, separate Init (§10.14.1).
    esp_err_t Init(i2cbus::Bus &bus);

    bool Present() const { return present_; }

    // One lease for the whole read, so the numbers are a snapshot rather than
    // five values from five different moments (§10.14.3 — this is what a lease
    // is for).
    esp_err_t Read(Status *out);

    static const char *ChargeStateName(uint8_t code);

   private:
    i2cbus::Bus *bus_ = nullptr;
    bool present_ = false;
};

}  // namespace pmic
