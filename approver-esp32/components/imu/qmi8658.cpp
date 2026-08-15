#include "qmi8658.h"

#include <cmath>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace imu {

namespace {

constexpr const char *TAG = "qmi8658";

// Table 22 (the UI register overview) and Table 29 (the output registers).
constexpr uint8_t kRegWhoAmI = 0x00;
constexpr uint8_t kRegRevision = 0x01;
constexpr uint8_t kRegCtrl1 = 0x02;
constexpr uint8_t kRegCtrl2 = 0x03;  // accelerometer: range + rate
constexpr uint8_t kRegCtrl3 = 0x04;  // gyroscope: range + rate
constexpr uint8_t kRegCtrl5 = 0x06;  // low-pass filters
constexpr uint8_t kRegCtrl7 = 0x08;  // which sensors are on
constexpr uint8_t kRegStatus0 = 0x2E;
constexpr uint8_t kRegTempL = 0x33;  // then AX_L..AZ_H, GX_L..GZ_H, contiguous
constexpr uint8_t kRegReset = 0x60;

// CTRL1: address auto-increment on, little-endian reads, 4-wire SPI (ignored on
// I²C), internal oscillator enabled. Written explicitly rather than OR-ed into
// whatever was there: after the soft reset below there is nothing to preserve,
// and the two bits that matter are exactly the two the defaults get wrong.
constexpr uint8_t kCtrl1AddrAutoIncrement = 1 << 6;
constexpr uint8_t kCtrl1BigEndian = 1 << 5;

// **These two are measured, not documented.** Rev 0.9 calls CTRL1 bits 4:1
// reserved, while its own revision history says it "updated the INT1/INT2
// enable bit in CTRL1" — so the table and the changelog disagree. Setting these
// two on this board makes INT2 start pulsing at the output data rate and
// clearing them stops it, which settles it: they are the pin enables, bit 4 for
// INT1 and bit 3 for INT2. Left off by default, because §10.13 gives this chip
// no job and a pin toggling 235 times a second for nobody is not free.
constexpr uint8_t kCtrl1Int1Enable = 1 << 4;
constexpr uint8_t kCtrl1Int2Enable = 1 << 3;

// CTRL7, Table 26.
constexpr uint8_t kCtrl7AccelEnable = 1 << 0;
constexpr uint8_t kCtrl7GyroEnable = 1 << 1;

// CTRL5: enable both filters at mode 00 (2.66 % of ODR).
constexpr uint8_t kCtrl5AccelLpfEnable = 1 << 0;
constexpr uint8_t kCtrl5GyroLpfEnable = 1 << 4;

// STATUS0, Table 28.
constexpr uint8_t kStatus0AccelData = 1 << 0;
constexpr uint8_t kStatus0GyroData = 1 << 1;

// Written to 0x60 to soft-reset from any mode, per the datasheet's §5.8.
constexpr uint8_t kResetCommand = 0xB0;

// The chip's full scales, in the order of the enums, so a range converts to a
// sensitivity without a switch. 32768 counts across the full scale.
constexpr float kAccelFullScaleG[] = {2.0f, 4.0f, 8.0f, 16.0f};
constexpr float kGyroFullScaleDps[] = {16.0f, 32.0f, 64.0f, 128.0f,
                                       256.0f, 512.0f, 1024.0f, 2048.0f};

int16_t Combine(const uint8_t *bytes) {
    // Little-endian, which is what CTRL1's BE bit is cleared for.
    return static_cast<int16_t>(static_cast<uint16_t>(bytes[0]) |
                                (static_cast<uint16_t>(bytes[1]) << 8));
}

}  // namespace

esp_err_t Qmi8658::Identify(uint8_t address) {
    auto lease = bus_->Acquire();
    if (!lease) {
        return ESP_ERR_TIMEOUT;
    }

    uint8_t who = 0;
    esp_err_t err = lease.ReadRegister(address, kRegWhoAmI, &who, 1);
    if (err != ESP_OK) {
        return err;
    }
    if (who != kChipId) {
        // Something answered, but it is not this chip. Worth its own error:
        // "nothing at 0x6b" and "a stranger at 0x6b" are different problems.
        ESP_LOGW(TAG, "0x%02x answered with 0x%02x, expected 0x%02x", address, who, kChipId);
        return ESP_ERR_NOT_SUPPORTED;
    }

    return lease.ReadRegister(address, kRegRevision, &revision_, 1);
}

esp_err_t Qmi8658::Configure(const Config &config) {
    // **Two leases, split exactly at the wait**, and that is §10.14.3 rather
    // than taste: nothing sleeps while holding the bus. Fifteen milliseconds of
    // held wire is a dropped touch read and a skipped clock tick for nothing —
    // and it is invisible on hardware, because it works either way. The host
    // tests are what caught it, here and in the codec.
    {
        auto lease = bus_->Acquire();
        if (!lease) {
            return ESP_ERR_TIMEOUT;
        }

        // The reset comes first and the configuration after it, never the other
        // way round: 0xB0 puts every CTRL register back to its default, so a
        // reset that followed the writes would silently discard them.
        const esp_err_t err = lease.WriteRegister(address_, kRegReset, kResetCommand);
        if (err != ESP_OK) {
            return err;
        }
    }

    // The datasheet gives no reset time; the vendor drivers wait ~10 ms and the
    // chip is unreachable until it is done, so this is a wait, not a delay.
    vTaskDelay(pdMS_TO_TICKS(15));

    auto lease = bus_->Acquire();
    if (!lease) {
        return ESP_ERR_TIMEOUT;
    }

    // Everything below is one uninterrupted sequence: the chip is sampling
    // against whatever is written by the time CTRL7 turns the sensors on.
    esp_err_t err = ESP_OK;

    uint8_t ctrl1 = kCtrl1AddrAutoIncrement;
    ctrl1 &= static_cast<uint8_t>(~kCtrl1BigEndian);
    if (config.interrupt_pins) {
        ctrl1 |= static_cast<uint8_t>(kCtrl1Int1Enable | kCtrl1Int2Enable);
    }
    err = lease.WriteRegister(address_, kRegCtrl1, ctrl1);
    if (err != ESP_OK) {
        return err;
    }

    const uint8_t ctrl2 = static_cast<uint8_t>(static_cast<uint8_t>(config.accel_range) << 4) |
                          static_cast<uint8_t>(config.accel_rate);
    err = lease.WriteRegister(address_, kRegCtrl2, ctrl2);
    if (err != ESP_OK) {
        return err;
    }

    const uint8_t ctrl3 = static_cast<uint8_t>(static_cast<uint8_t>(config.gyro_range) << 4) |
                          static_cast<uint8_t>(config.gyro_rate);
    err = lease.WriteRegister(address_, kRegCtrl3, ctrl3);
    if (err != ESP_OK) {
        return err;
    }

    const uint8_t ctrl5 = config.low_pass
                              ? static_cast<uint8_t>(kCtrl5AccelLpfEnable | kCtrl5GyroLpfEnable)
                              : 0;
    err = lease.WriteRegister(address_, kRegCtrl5, ctrl5);
    if (err != ESP_OK) {
        return err;
    }

    // Both sensors last, so nothing starts sampling against a half-written
    // configuration.
    return lease.WriteRegister(address_, kRegCtrl7,
                               static_cast<uint8_t>(kCtrl7AccelEnable | kCtrl7GyroEnable));
}

esp_err_t Qmi8658::Init(i2cbus::Bus &bus, const Config &config) {
    bus_ = &bus;
    present_ = false;

    // SA0 low is 0x6B and the vendor's own example for this board uses the
    // "high" constant of its driver, which resolves to it — but the wiring is
    // not written down anywhere this firmware can check, so both are tried and
    // the one that answers is reported.
    esp_err_t err = Identify(kAddressSa0Low);
    if (err == ESP_OK) {
        address_ = kAddressSa0Low;
    } else {
        const esp_err_t first = err;
        err = Identify(kAddressSa0High);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "no QMI8658C: 0x%02x %s, 0x%02x %s", kAddressSa0Low,
                     esp_err_to_name(first), kAddressSa0High, esp_err_to_name(err));
            return err;
        }
        address_ = kAddressSa0High;
    }

    err = Configure(config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "configuration failed: %s", esp_err_to_name(err));
        return err;
    }

    config_ = config;
    accel_lsb_per_g_ = 32768.0f / kAccelFullScaleG[static_cast<uint8_t>(config.accel_range)];
    gyro_lsb_per_dps_ = 32768.0f / kGyroFullScaleDps[static_cast<uint8_t>(config.gyro_range)];
    present_ = true;

    ESP_LOGI(TAG, "QMI8658C at 0x%02x, revision 0x%02x", address_, revision_);
    return ESP_OK;
}

esp_err_t Qmi8658::Read(Sample *out) {
    if (out == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!present_) {
        return ESP_ERR_INVALID_STATE;
    }

    auto lease = bus_->Acquire();
    if (!lease) {
        return ESP_ERR_TIMEOUT;
    }

    uint8_t status = 0;
    esp_err_t err = lease.ReadRegister(address_, kRegStatus0, &status, 1);
    if (err != ESP_OK) {
        return err;
    }

    // TEMP_L .. GZ_H: fourteen contiguous registers, one transfer, so the six
    // axes describe one moment. This is the read that needs CTRL1's
    // auto-increment — without it every byte would be TEMP_L again.
    uint8_t raw[14] = {};
    err = lease.ReadRegister(address_, kRegTempL, raw, sizeof(raw));
    if (err != ESP_OK) {
        return err;
    }

    out->celsius = static_cast<float>(Combine(&raw[0])) / 256.0f;
    for (int axis = 0; axis < 3; ++axis) {
        out->accel_g[axis] = static_cast<float>(Combine(&raw[2 + axis * 2])) / accel_lsb_per_g_;
        out->gyro_dps[axis] = static_cast<float>(Combine(&raw[8 + axis * 2])) / gyro_lsb_per_dps_;
    }
    out->accel_fresh = (status & kStatus0AccelData) != 0;
    out->gyro_fresh = (status & kStatus0GyroData) != 0;
    return ESP_OK;
}

void Qmi8658::Tilt(const Sample &sample, float *pitch_deg, float *roll_deg) {
    const float x = sample.accel_g[0];
    const float y = sample.accel_g[1];
    const float z = sample.accel_g[2];
    constexpr float kRadToDeg = 57.29577951308232f;

    if (pitch_deg != nullptr) {
        *pitch_deg = atan2f(-x, sqrtf(y * y + z * z)) * kRadToDeg;
    }
    if (roll_deg != nullptr) {
        *roll_deg = atan2f(y, z) * kRadToDeg;
    }
}

float Qmi8658::Magnitude(const Sample &sample) {
    return sqrtf(sample.accel_g[0] * sample.accel_g[0] + sample.accel_g[1] * sample.accel_g[1] +
                 sample.accel_g[2] * sample.accel_g[2]);
}

}  // namespace imu
