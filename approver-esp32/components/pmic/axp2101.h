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

// The vendor's driver opens this chip at 100 kHz while the rest of the bus runs
// faster. Kept because they chose it deliberately for this board, and because
// the per-device clock costs nothing (§10.14.3).
inline constexpr uint32_t kClockHz = 100000;

// The register field values, with the numbers XPowersLib's enums resolve to.
// `ChargeCurrent` is the one worth staring at: it is not a dense enum — it
// jumps from 0 mA at 0 straight to 100 mA at 4.
enum class VbusCurrentLimit : uint8_t {
    k100mA = 0,
    k500mA = 1,
    k900mA = 2,
    k1000mA = 3,
    k1500mA = 4,
    k2000mA = 5,
};

enum class PrechargeCurrent : uint8_t {
    k0mA = 0,
    k25mA = 1,
    k50mA = 2,
    k75mA = 3,
};

enum class ChargeCurrent : uint8_t {
    k0mA = 0,
    k100mA = 4,
    k125mA = 5,
    k150mA = 6,
    k175mA = 7,
    k200mA = 8,
    k300mA = 9,
    k400mA = 10,
    k500mA = 11,
    k600mA = 12,
    k700mA = 13,
    k800mA = 14,
    k900mA = 15,
    k1000mA = 16,
};

enum class TerminationCurrent : uint8_t {
    k0mA = 0,
    k25mA = 1,
    k50mA = 2,
    k75mA = 3,
    k100mA = 4,
};

// What `Init` writes. The defaults are Waveshare's for this board, from
// `Custom_PmicRegisterInit()` in their `pmicpower` component — not this
// author's judgement about someone else's battery.
struct Config {
    uint16_t rail_mv = 3300;  // DC1 and ALDO1..4, written only if they differ
    VbusCurrentLimit vbus_limit = VbusCurrentLimit::k2000mA;
    PrechargeCurrent precharge = PrechargeCurrent::k50mA;
    ChargeCurrent charge = ChargeCurrent::k500mA;
    TerminationCurrent termination = TerminationCurrent::k50mA;
};

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
    bool aldo2_enabled;   // the audio amplifier's rail (§10.1)
    bool aldo3_enabled;   // the panel's reset rail (§10.1)
    uint16_t dc1_mv;      // the C6's own supply
    uint16_t aldo2_mv;
    uint16_t aldo3_mv;

    // The PWRON key — `PWR` on the board, GPIO18, active low (§10.1). These
    // say what the *chip* does with it, which is not something the firmware
    // participates in: it happens whether or not any code is running.
    uint8_t press_on_code;      // 0..3 -> 128 ms / 512 ms / 1 s / 2 s
    uint8_t press_off_code;     // 0..3 -> 4 s / 6 s / 8 s / 10 s
    bool long_press_shutdown;   // COMMON_CONFIG bit 2: does the long press act?
    uint8_t power_on_source;    // PWRON_STATUS, why the board is awake
};

// Names for the three fields above. `PowerOnSourceName` decodes the first bit
// set in PWRON_STATUS.
const char *PressOnTimeName(uint8_t code);
const char *PressOffTimeName(uint8_t code);
const char *PowerOnSourceName(uint8_t status);

class Axp2101 {
   public:
    Axp2101() = default;
    Axp2101(const Axp2101 &) = delete;
    Axp2101 &operator=(const Axp2101 &) = delete;

    // Identifies the chip, silences the TS pin, turns on the ADC channels this
    // firmware reads, and applies `Config`. Trivial constructor, separate Init
    // (§10.14.1). All of it happens under one lease.
    esp_err_t Init(i2cbus::Bus &bus, const Config &config = Config{});

    bool Present() const { return present_; }

    // One lease for the whole read, so the numbers are a snapshot rather than
    // five values from five different moments (§10.14.3 — this is what a lease
    // is for).
    esp_err_t Read(Status *out);

    // ALDO3 is the panel's reset rail and ALDO2 the audio amplifier's (§10.1) —
    // which is why this driver has to exist before the display does. `Init`
    // sets their voltage and deliberately leaves them **off**: whoever owns the
    // panel or the codec turns its own rail on.
    esp_err_t SetAldo2(bool on);
    esp_err_t SetAldo3(bool on);

    // Cuts power to the board — the AXP2101's soft power-off, after which only
    // the PWR button or a charger brings it back.
    //
    // **Refuses while USB is connected**, returning ESP_ERR_INVALID_STATE and
    // writing nothing. VBUS is a power-on source for this chip, so a shutdown
    // with the cable in is a shutdown the hardware undoes: what the operator
    // sees is not a device switching off but a device rebooting, which on a
    // thing that sits on a desk reads as a crash. Refusing says the true thing
    // — "unplug it first" — instead of performing a power-off that does not
    // happen.
    esp_err_t PowerOff();

    static const char *ChargeStateName(uint8_t code);

   private:
    esp_err_t SetRail(uint8_t bit, bool on, const char *name);

    i2cbus::Bus *bus_ = nullptr;
    bool present_ = false;
};

}  // namespace pmic
