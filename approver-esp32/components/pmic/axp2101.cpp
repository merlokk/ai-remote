#include "axp2101.h"

#include "esp_log.h"

namespace pmic {

namespace {

constexpr const char *TAG = "axp2101";

// Registers, from XPowersLib's AXP2101Constants.h (see the header for why that
// is the source rather than this file's author).
constexpr uint8_t kRegStatus1 = 0x00;
constexpr uint8_t kRegStatus2 = 0x01;
constexpr uint8_t kRegChipId = 0x03;
constexpr uint8_t kRegAdcChannelCtrl = 0x30;
constexpr uint8_t kRegAdcBatteryHigh = 0x34;  // 0x34/0x35, 13 bits, mV
constexpr uint8_t kRegAdcVbusHigh = 0x38;     // 0x38/0x39, 14 bits, mV
constexpr uint8_t kRegAdcSystemHigh = 0x3A;   // 0x3A/0x3B, 14 bits, mV
constexpr uint8_t kRegAdcDieHigh = 0x3C;      // 0x3C/0x3D, 14 bits, raw
constexpr uint8_t kRegBatteryPercent = 0xA4;
constexpr uint8_t kRegCommonConfig = 0x10;  // bit 0 is the soft power-off
constexpr uint8_t kRegVbusCurrentLimit = 0x16;
constexpr uint8_t kRegPwronStatus = 0x20;
constexpr uint8_t kRegKeyLevelCtrl = 0x27;  // press-on time 1:0, press-off time 3:2
// The four bits of 0x27 this driver owns. Everything above them is the chip's
// and is read back and preserved, which is the rule every write here follows.
constexpr uint8_t kKeyTimeMask = 0x0F;
constexpr uint8_t kRegTsPinCtrl = 0x50;
constexpr uint8_t kRegPrechargeCurrent = 0x61;
constexpr uint8_t kRegChargeCurrent = 0x62;
constexpr uint8_t kRegTerminationCurrent = 0x63;
constexpr uint8_t kRegDcOnOff = 0x80;
constexpr uint8_t kRegDc1Voltage = 0x82;
constexpr uint8_t kRegLdoOnOff0 = 0x90;
// LDO_VOL0..3 are ALDO1..ALDO4, one register apart.
constexpr uint8_t kRegAldo1Voltage = 0x92;

// Rail encodings: (mV - min) / step in the low five bits.
constexpr uint16_t kDc1MinMv = 1500;
constexpr uint16_t kDc1MaxMv = 3400;
constexpr uint16_t kAldoMinMv = 500;
constexpr uint16_t kAldoMaxMv = 3500;
constexpr uint16_t kRailStepMv = 100;
constexpr uint8_t kRailVoltageMask = 0x1F;

// LDO_ONOFF_CTRL0: one bit per ALDO, ALDO1 at bit 0.
constexpr uint8_t kAldo2Bit = 1 << 1;
constexpr uint8_t kAldo3Bit = 1 << 2;
// DC_ONOFF_DVM_CTRL bit 0 is DCDC1.

// ADC_CHANNEL_CTRL bit 1 is the TS pin's channel.
constexpr uint8_t kAdcTsChannel = 1 << 1;

// COMMON_CONFIG bit 0: XPowersLib's `shutdown()` sets exactly this.
constexpr uint8_t kSoftPowerOffBit = 1 << 0;
// COMMON_CONFIG bit 2: whether a long press on PWRON actually shuts the PMIC
// down. Without it the chip measures the long press and does nothing.
constexpr uint8_t kPwronShutsPmicBit = 1 << 2;

// STATUS1 bit 3: a battery is connected. Bit 5: VBUS is good.
constexpr uint8_t kStatus1BatteryPresent = 1 << 3;
constexpr uint8_t kStatus1VbusGood = 1 << 5;
// STATUS2 bit 3 set means VBUS is *not* usable, even when it is good.
constexpr uint8_t kStatus2VbusUnusable = 1 << 3;

// ADC channels this firmware reads: battery (0), VBUS (2), system (3), die (4).
constexpr uint8_t kAdcChannels = (1 << 0) | (1 << 2) | (1 << 3) | (1 << 4);

// XPOWERS_AXP2101_CONVERSION, verbatim.
float DieCelsius(uint16_t raw) { return 22.0f + (7274.0f - static_cast<float>(raw)) / 20.0f; }

uint16_t Combine(uint8_t high, uint8_t low, uint8_t high_mask) {
    return static_cast<uint16_t>(((high & high_mask) << 8) | low);
}

// Two bits in two registers decide it, and both matter: STATUS1 says the VBUS
// rail is good, STATUS2 bit 3 says it is nonetheless unusable. Kept as a pure
// function of the two bytes — it is the piece of this driver a host test can
// reach without a bus (§10.11).
bool VbusPresent(uint8_t status1, uint8_t status2) {
    return ((status1 & kStatus1VbusGood) != 0) && ((status2 & kStatus2VbusUnusable) == 0);
}

// Read-modify-write under a lease the caller already holds. `keep` is the mask
// of bits to leave alone; every field on this chip shares a register with
// something else, so a blind write is a way to switch off a rail by accident.
esp_err_t Update(i2cbus::Lease &lease, uint8_t reg, uint8_t keep, uint8_t value) {
    uint8_t current = 0;
    const esp_err_t err = lease.ReadRegister(kAddress, reg, &current, 1);
    if (err != ESP_OK) {
        return err;
    }
    return lease.WriteRegister(kAddress, reg,
                               static_cast<uint8_t>((current & keep) | value));
}

// **The power key, asserted rather than read.** Two registers decide whether the
// button on the case can switch this board on and off (§10.1), and until this
// existed the driver printed what it found in them and believed it.
//
// The one thing this must never do is write COMMON_CONFIG bit 0. That bit is the
// soft power-off, and a read-modify-write that preserved it would switch the
// board off while configuring it — a device that goes dark during `Init` and
// comes back only to do it again. So the write **clears** it explicitly, and
// `PowerOff` stays the only thing in this driver that ever sets it.
esp_err_t SetPowerKey(i2cbus::Lease &lease, uint8_t press_on, uint8_t press_off,
                      bool long_press_shutdown) {
    uint8_t key = 0;
    esp_err_t err = lease.ReadRegister(kAddress, kRegKeyLevelCtrl, &key, 1);
    if (err != ESP_OK) {
        return err;
    }
    const uint8_t wanted_key =
        static_cast<uint8_t>((key & ~kKeyTimeMask) | (press_on & 0x03) |
                             static_cast<uint8_t>((press_off & 0x03) << 2));
    if (wanted_key != key) {
        // Loud, because it means the chip came up holding something this
        // firmware did not put there — which is the whole reason to look.
        ESP_LOGW(TAG, "power key was 0x%02x, setting 0x%02x (on %s, off %s)", key, wanted_key,
                 PressOnTimeName(press_on & 0x03), PressOffTimeName(press_off & 0x03));
        err = lease.WriteRegister(kAddress, kRegKeyLevelCtrl, wanted_key);
        if (err != ESP_OK) {
            return err;
        }
    }

    uint8_t common = 0;
    err = lease.ReadRegister(kAddress, kRegCommonConfig, &common, 1);
    if (err != ESP_OK) {
        return err;
    }
    const bool acts = (common & kPwronShutsPmicBit) != 0;
    const bool power_off_pending = (common & kSoftPowerOffBit) != 0;
    if (acts == long_press_shutdown && !power_off_pending) {
        return ESP_OK;
    }
    if (power_off_pending) {
        // Reading this bit set at boot is odd enough to say out loud: it should
        // have taken the board off, so finding it here means the write did not
        // happen or the chip was powered through it.
        ESP_LOGW(TAG, "the soft power-off bit was set at boot; clearing it");
    }
    uint8_t wanted_common = static_cast<uint8_t>(common & ~kSoftPowerOffBit);
    wanted_common = long_press_shutdown
                        ? static_cast<uint8_t>(wanted_common | kPwronShutsPmicBit)
                        : static_cast<uint8_t>(wanted_common & ~kPwronShutsPmicBit);
    if (acts != long_press_shutdown) {
        ESP_LOGW(TAG, "long press %s the chip down; setting it to %s",
                 acts ? "shut" : "did not shut", long_press_shutdown ? "shut" : "not shut");
    }
    return lease.WriteRegister(kAddress, kRegCommonConfig, wanted_common);
}

// Sets a rail's voltage only when it is not already there — the vendor's
// `if (getXxxVoltage() != 3300)` guard, kept for a real reason: DCDC1 supplies
// the C6 itself, and a write that changes nothing is a write that cannot
// disturb it.
esp_err_t SetRailVoltage(i2cbus::Lease &lease, uint8_t reg, uint16_t min_mv,
                         uint16_t max_mv, uint16_t millivolt, const char *name) {
    if (millivolt < min_mv || millivolt > max_mv || (millivolt % kRailStepMv) != 0) {
        ESP_LOGE(TAG, "%s: %u mV is outside %u..%u in %u mV steps", name, millivolt, min_mv,
                 max_mv, kRailStepMv);
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t current = 0;
    esp_err_t err = lease.ReadRegister(kAddress, reg, &current, 1);
    if (err != ESP_OK) {
        return err;
    }

    const uint8_t wanted = static_cast<uint8_t>((millivolt - min_mv) / kRailStepMv);
    if ((current & kRailVoltageMask) == wanted) {
        return ESP_OK;
    }

    ESP_LOGW(TAG, "%s: %u -> %u mV", name,
             static_cast<unsigned>((current & kRailVoltageMask) * kRailStepMv + min_mv),
             millivolt);
    return lease.WriteRegister(
        kAddress, reg, static_cast<uint8_t>((current & ~kRailVoltageMask) | wanted));
}

}  // namespace

const char *PressOnTimeName(uint8_t code) {
    static const char *names[] = {"128 ms", "512 ms", "1 s", "2 s"};
    return names[code & 0x03];
}

const char *PressOffTimeName(uint8_t code) {
    static const char *names[] = {"4 s", "6 s", "8 s", "10 s"};
    return names[code & 0x03];
}

const char *PowerOnSourceName(uint8_t status) {
    // PWRON_STATUS, one bit per reason. Lowest set bit wins; several can be
    // set at once and the order below is the useful one for a desk device.
    if (status & (1 << 0)) {
        return "PWR button";
    }
    if (status & (1 << 2)) {
        return "USB plugged in";
    }
    if (status & (1 << 4)) {
        return "battery inserted";
    }
    if (status & (1 << 3)) {
        return "battery charging";
    }
    if (status & (1 << 1)) {
        return "IRQ pin";
    }
    if (status & (1 << 5)) {
        return "EN held high";
    }
    return "unknown";
}

const char *Axp2101::ChargeStateName(uint8_t code) {
    switch (static_cast<ChargeState>(code)) {
        case ChargeState::kTrickle:
            return "trickle";
        case ChargeState::kPreCharge:
            return "pre-charge";
        case ChargeState::kConstantCurrent:
            return "constant current";
        case ChargeState::kConstantVoltage:
            return "constant voltage";
        case ChargeState::kDone:
            return "done";
        case ChargeState::kNotCharging:
            return "not charging";
        default:
            return "unknown";
    }
}

esp_err_t Axp2101::Init(i2cbus::Bus &bus, const Config &config) {
    bus_ = &bus;
    present_ = false;

    // Before the lease, not under it: this takes the bus itself.
    esp_err_t err = bus.AddDevice(kAddress, kClockHz);
    if (err != ESP_OK) {
        return err;
    }

    auto lease = bus.Acquire();
    if (!lease) {
        return ESP_ERR_TIMEOUT;
    }

    uint8_t id = 0;
    err = lease.ReadRegister(kAddress, kRegChipId, &id, 1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "no answer at 0x%02x: %s", kAddress, esp_err_to_name(err));
        return err;
    }
    if (id != kChipId) {
        // Not a fatal error to the caller, but it is not this chip, so nothing
        // below should be believed.
        ESP_LOGE(TAG, "chip id 0x%02x, expected 0x%02x", id, kChipId);
        return ESP_ERR_NOT_SUPPORTED;
    }

    // **The power key first, before anything else is configured.** Everything
    // below this can fail and leave a board that boots and complains; this is
    // the one register whose being wrong means the button on the case does not
    // bring the board back at all, so it is written while the bus is known good
    // and before any other write can return early.
    err = SetPowerKey(lease, static_cast<uint8_t>(config.press_on),
                      static_cast<uint8_t>(config.press_off), config.long_press_shutdown);
    if (err != ESP_OK) {
        return err;
    }

    // **Silence the TS pin.** XPowersLib does this inside `begin()`, and its own
    // comment says why: with TS measurement on, the pin is read as a battery
    // thermistor and *affects the charger*. This board's charging works either
    // way today, but the vendor changes the default deliberately and a charger
    // that stops on a reading nobody wired is a bad afternoon.
    err = Update(lease, kRegTsPinCtrl, 0xF0, 0x10);
    if (err != ESP_OK) {
        return err;
    }

    // The ADC channels this firmware reads, and the TS channel cleared in the
    // same write — one read-modify-write instead of two, and the reason the
    // lease is held across the whole of Init rather than taken per call
    // (§10.14.3).
    uint8_t channels = 0;
    err = lease.ReadRegister(kAddress, kRegAdcChannelCtrl, &channels, 1);
    if (err != ESP_OK) {
        return err;
    }
    channels = static_cast<uint8_t>((channels | kAdcChannels) & ~kAdcTsChannel);
    err = lease.WriteRegister(kAddress, kRegAdcChannelCtrl, channels);
    if (err != ESP_OK) {
        return err;
    }

    // How much may be drawn from USB. The default is lower, and the panel plus
    // the radio is what spends it.
    err = Update(lease, kRegVbusCurrentLimit, 0xF8,
                 static_cast<uint8_t>(config.vbus_limit));
    if (err != ESP_OK) {
        return err;
    }

    // The rails. DCDC1 supplies the C6; ALDO3 is the panel's reset and ALDO2
    // the amplifier (§10.1). Voltage only — enabling ALDO2/ALDO3 belongs to
    // whoever owns the panel and the codec.
    err = SetRailVoltage(lease, kRegDc1Voltage, kDc1MinMv, kDc1MaxMv, config.rail_mv, "dc1");
    if (err != ESP_OK) {
        return err;
    }
    for (int i = 0; i < 4; ++i) {
        const char *names[] = {"aldo1", "aldo2", "aldo3", "aldo4"};
        err = SetRailVoltage(lease, static_cast<uint8_t>(kRegAldo1Voltage + i), kAldoMinMv,
                             kAldoMaxMv, config.rail_mv, names[i]);
        if (err != ESP_OK) {
            return err;
        }
    }

    // Charging, with this board's battery in mind rather than the chip's
    // power-on defaults. Each value shares its register with something else,
    // hence the masks.
    err = Update(lease, kRegPrechargeCurrent, 0xFC, static_cast<uint8_t>(config.precharge));
    if (err != ESP_OK) {
        return err;
    }
    err = Update(lease, kRegChargeCurrent, 0xE0, static_cast<uint8_t>(config.charge));
    if (err != ESP_OK) {
        return err;
    }
    err = Update(lease, kRegTerminationCurrent, 0xF0,
                 static_cast<uint8_t>(config.termination));
    if (err != ESP_OK) {
        return err;
    }

    present_ = true;
    ESP_LOGI(TAG, "found at 0x%02x, chip id 0x%02x, ADC 0x%02x, rails %u mV, charge 500 mA",
             kAddress, id, channels, config.rail_mv);
    return ESP_OK;
}

esp_err_t Axp2101::SetAldo2(bool on) { return SetRail(kAldo2Bit, on, "aldo2"); }

esp_err_t Axp2101::SetAldo3(bool on) { return SetRail(kAldo3Bit, on, "aldo3"); }

esp_err_t Axp2101::PowerOff() {
    if (!present_ || bus_ == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    auto lease = bus_->Acquire();
    if (!lease) {
        return ESP_ERR_TIMEOUT;
    }

    // The check and the write are under the same lease, so nothing can plug a
    // cable in between them as far as this chip's registers are concerned —
    // which is the other half of what §10.14.3's lease is for.
    uint8_t status[2] = {};
    esp_err_t err = lease.ReadRegister(kAddress, kRegStatus1, &status[0], 1);
    if (err != ESP_OK) {
        return err;
    }
    err = lease.ReadRegister(kAddress, kRegStatus2, &status[1], 1);
    if (err != ESP_OK) {
        return err;
    }

    if (VbusPresent(status[0], status[1])) {
        // Refuse, and write nothing. See the header: VBUS powers this chip back
        // on, so the shutdown would look like a reboot.
        ESP_LOGW(TAG, "power off refused: USB is connected");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGW(TAG, "powering off");
    return Update(lease, kRegCommonConfig, 0xFF, kSoftPowerOffBit);
}

esp_err_t Axp2101::SetRail(uint8_t bit, bool on, const char *name) {
    if (!present_ || bus_ == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    auto lease = bus_->Acquire();
    if (!lease) {
        return ESP_ERR_TIMEOUT;
    }

    const esp_err_t err =
        Update(lease, kRegLdoOnOff0, static_cast<uint8_t>(~bit), on ? bit : 0);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "%s %s", name, on ? "on" : "off");
    }
    return err;
}

esp_err_t Axp2101::Read(Status *out) {
    if (out == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!present_ || bus_ == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    auto lease = bus_->Acquire();
    if (!lease) {
        return ESP_ERR_TIMEOUT;
    }

    uint8_t status[2] = {};
    esp_err_t err = lease.ReadRegister(kAddress, kRegStatus1, &status[0], 1);
    if (err != ESP_OK) {
        return err;
    }
    err = lease.ReadRegister(kAddress, kRegStatus2, &status[1], 1);
    if (err != ESP_OK) {
        return err;
    }

    *out = Status{};
    out->battery_present = (status[0] & kStatus1BatteryPresent) != 0;
    out->vbus_present = VbusPresent(status[0], status[1]);
    out->charging = ((status[1] >> 5) & 0x03) == 0x01;
    out->discharging = ((status[1] >> 5) & 0x03) == 0x02;
    out->charge_code = status[1] & 0x07;
    out->battery_percent = -1;

    uint8_t rails = 0;
    if (lease.ReadRegister(kAddress, kRegLdoOnOff0, &rails, 1) == ESP_OK) {
        out->aldo2_enabled = (rails & kAldo2Bit) != 0;
        out->aldo3_enabled = (rails & kAldo3Bit) != 0;
    }

    uint8_t key = 0;
    if (lease.ReadRegister(kAddress, kRegKeyLevelCtrl, &key, 1) == ESP_OK) {
        out->press_on_code = key & 0x03;
        out->press_off_code = (key >> 2) & 0x03;
    }
    uint8_t common = 0;
    if (lease.ReadRegister(kAddress, kRegCommonConfig, &common, 1) == ESP_OK) {
        out->long_press_shutdown = (common & kPwronShutsPmicBit) != 0;
    }
    lease.ReadRegister(kAddress, kRegPwronStatus, &out->power_on_source, 1);

    uint8_t rail = 0;
    if (lease.ReadRegister(kAddress, kRegDc1Voltage, &rail, 1) == ESP_OK) {
        out->dc1_mv = static_cast<uint16_t>((rail & kRailVoltageMask) * kRailStepMv + kDc1MinMv);
    }
    if (lease.ReadRegister(kAddress, kRegAldo1Voltage + 1, &rail, 1) == ESP_OK) {
        out->aldo2_mv =
            static_cast<uint16_t>((rail & kRailVoltageMask) * kRailStepMv + kAldoMinMv);
    }
    if (lease.ReadRegister(kAddress, kRegAldo1Voltage + 2, &rail, 1) == ESP_OK) {
        out->aldo3_mv =
            static_cast<uint16_t>((rail & kRailVoltageMask) * kRailStepMv + kAldoMinMv);
    }

    uint8_t pair[2] = {};

    // Battery is 13 bits (H5L8), the rest 14 (H6L8) — the widths are not
    // uniform and getting one wrong shows up as a plausible wrong voltage.
    if (out->battery_present) {
        err = lease.ReadRegister(kAddress, kRegAdcBatteryHigh, pair, sizeof(pair));
        if (err != ESP_OK) {
            return err;
        }
        out->battery_mv = Combine(pair[0], pair[1], 0x1F);

        uint8_t percent = 0;
        if (lease.ReadRegister(kAddress, kRegBatteryPercent, &percent, 1) == ESP_OK &&
            percent <= 100) {
            out->battery_percent = percent;
        }
    }

    if (out->vbus_present) {
        err = lease.ReadRegister(kAddress, kRegAdcVbusHigh, pair, sizeof(pair));
        if (err != ESP_OK) {
            return err;
        }
        out->vbus_mv = Combine(pair[0], pair[1], 0x3F);
    }

    err = lease.ReadRegister(kAddress, kRegAdcSystemHigh, pair, sizeof(pair));
    if (err != ESP_OK) {
        return err;
    }
    out->system_mv = Combine(pair[0], pair[1], 0x3F);

    err = lease.ReadRegister(kAddress, kRegAdcDieHigh, pair, sizeof(pair));
    if (err != ESP_OK) {
        return err;
    }
    out->die_celsius = DieCelsius(Combine(pair[0], pair[1], 0x3F));

    return ESP_OK;
}

}  // namespace pmic
