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

}  // namespace

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

esp_err_t Axp2101::Init(i2cbus::Bus &bus) {
    bus_ = &bus;
    present_ = false;

    auto lease = bus.Acquire();
    if (!lease) {
        return ESP_ERR_TIMEOUT;
    }

    uint8_t id = 0;
    esp_err_t err = lease.ReadRegister(kAddress, kRegChipId, &id, 1);
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

    // Read-modify-write, and the reason the lease is held across both halves
    // rather than taken per call (§10.14.3).
    uint8_t channels = 0;
    err = lease.ReadRegister(kAddress, kRegAdcChannelCtrl, &channels, 1);
    if (err != ESP_OK) {
        return err;
    }
    err = lease.WriteRegister(kAddress, kRegAdcChannelCtrl,
                              static_cast<uint8_t>(channels | kAdcChannels));
    if (err != ESP_OK) {
        return err;
    }

    present_ = true;
    ESP_LOGI(TAG, "found at 0x%02x, chip id 0x%02x, ADC channels 0x%02x", kAddress, id,
             static_cast<unsigned>(channels | kAdcChannels));
    return ESP_OK;
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
    out->vbus_present =
        ((status[0] & kStatus1VbusGood) != 0) && ((status[1] & kStatus2VbusUnusable) == 0);
    out->charging = ((status[1] >> 5) & 0x03) == 0x01;
    out->discharging = ((status[1] >> 5) & 0x03) == 0x02;
    out->charge_code = status[1] & 0x07;
    out->battery_percent = -1;

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
