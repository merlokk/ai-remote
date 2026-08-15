#include "pcf85063.h"

#include "esp_log.h"

namespace rtc {

namespace {

constexpr const char *TAG = "pcf85063";

// Datasheet Table 5. The seven time counters are contiguous from 04h, which is
// what makes a single burst possible.
constexpr uint8_t kRegControl1 = 0x00;
constexpr uint8_t kRegSeconds = 0x04;
constexpr size_t kCounterCount = 7;  // seconds, minutes, hours, days, weekdays, months, years

// Control_1, Table 6.
constexpr uint8_t kControl1Stop = 1 << 5;

// Seconds, Table 19: bit 7 is OS — set means the oscillator stopped and clock
// integrity is *not* guaranteed.
constexpr uint8_t kSecondsOsFlag = 1 << 7;

// Field masks, Table 5. The unimplemented bits read as whatever they read as,
// so every field is masked rather than trusted.
constexpr uint8_t kSecondsMask = 0x7F;
constexpr uint8_t kMinutesMask = 0x7F;
constexpr uint8_t kHoursMask24 = 0x3F;
constexpr uint8_t kDaysMask = 0x3F;
constexpr uint8_t kWeekdaysMask = 0x07;
constexpr uint8_t kMonthsMask = 0x1F;

// The chip has no century. 2000 is the assumption, and it is the same one every
// driver for this part makes.
constexpr uint16_t kCentury = 2000;

uint8_t FromBcd(uint8_t value) {
    return static_cast<uint8_t>((value >> 4) * 10 + (value & 0x0F));
}

uint8_t ToBcd(uint8_t value) {
    return static_cast<uint8_t>(((value / 10) << 4) | (value % 10));
}

bool InRange(const DateTime &t) {
    return t.month >= 1 && t.month <= 12 && t.day >= 1 && t.day <= 31 && t.hour <= 23 &&
           t.minute <= 59 && t.second <= 59 && t.weekday <= 6 && t.year >= kCentury &&
           t.year <= kCentury + 99;
}

}  // namespace

esp_err_t Pcf85063::Init(i2cbus::Bus &bus) {
    bus_ = &bus;
    present_ = false;

    auto lease = bus.Acquire();
    if (!lease) {
        return ESP_ERR_TIMEOUT;
    }

    // No identity register on this part, so the question is only whether
    // something answers at 0x51.
    esp_err_t err = lease.Probe(kAddress);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "no answer at 0x%02x: %s", kAddress, esp_err_to_name(err));
        return err;
    }

    uint8_t control1 = 0;
    err = lease.ReadRegister(kAddress, kRegControl1, &control1, 1);
    if (err != ESP_OK) {
        return err;
    }

    // Control_1 is deliberately left alone. CAP_SEL (bit 0) picks the load
    // capacitance for the crystal that is soldered next to this chip, and 12_24
    // is already 24-hour after reset — writing either from here would be this
    // firmware guessing at someone else's hardware.
    present_ = true;
    ESP_LOGI(TAG, "found at 0x%02x, control_1 0x%02x", kAddress, control1);
    return ESP_OK;
}

esp_err_t Pcf85063::Read(DateTime *out, bool *valid) {
    if (out == nullptr || valid == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    *valid = false;
    if (!present_ || bus_ == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    auto lease = bus_->Acquire();
    if (!lease) {
        return ESP_ERR_TIMEOUT;
    }

    uint8_t raw[kCounterCount] = {};
    const esp_err_t err = lease.ReadRegister(kAddress, kRegSeconds, raw, sizeof(raw));
    if (err != ESP_OK) {
        return err;
    }

    *out = DateTime{};
    out->second = FromBcd(raw[0] & kSecondsMask);
    out->minute = FromBcd(raw[1] & kMinutesMask);
    out->hour = FromBcd(raw[2] & kHoursMask24);
    out->day = FromBcd(raw[3] & kDaysMask);
    out->weekday = static_cast<uint8_t>(raw[4] & kWeekdaysMask);
    out->month = FromBcd(raw[5] & kMonthsMask);
    out->year = static_cast<uint16_t>(kCentury + FromBcd(raw[6]));

    // Two separate reasons not to believe it, and they are worth keeping
    // separate from a transport error: the chip says its oscillator stopped, or
    // the BCD decodes to something that is not a date.
    if ((raw[0] & kSecondsOsFlag) != 0) {
        return ESP_OK;  // read fine, value not trustworthy
    }
    *valid = InRange(*out);
    return ESP_OK;
}

esp_err_t Pcf85063::Write(const DateTime &value) {
    if (!present_ || bus_ == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!InRange(value)) {
        return ESP_ERR_INVALID_ARG;
    }

    auto lease = bus_->Acquire();
    if (!lease) {
        return ESP_ERR_TIMEOUT;
    }

    uint8_t control1 = 0;
    esp_err_t err = lease.ReadRegister(kAddress, kRegControl1, &control1, 1);
    if (err != ESP_OK) {
        return err;
    }

    // Stop the clock across the write. The datasheet's warning is about the
    // counters incrementing between accesses; stopping it removes the race
    // rather than narrowing it.
    err = lease.WriteRegister(kAddress, kRegControl1,
                              static_cast<uint8_t>(control1 | kControl1Stop));
    if (err != ESP_OK) {
        return err;
    }

    // Seconds first, and its bit 7 written as 0 — that is what clears OS and
    // makes the clock trustworthy again.
    const uint8_t payload[1 + kCounterCount] = {
        kRegSeconds,
        ToBcd(value.second),
        ToBcd(value.minute),
        ToBcd(value.hour),
        ToBcd(value.day),
        static_cast<uint8_t>(value.weekday & kWeekdaysMask),
        ToBcd(value.month),
        ToBcd(static_cast<uint8_t>(value.year - kCentury)),
    };
    err = lease.Write(kAddress, payload, sizeof(payload));

    // Restart the clock whatever happened above: leaving it stopped because a
    // write failed would turn one bad write into a dead clock.
    const esp_err_t restart = lease.WriteRegister(
        kAddress, kRegControl1, static_cast<uint8_t>(control1 & ~kControl1Stop));

    return (err != ESP_OK) ? err : restart;
}

}  // namespace rtc
