#include "es8311.h"

#include <cinttypes>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace audio {

namespace {

constexpr const char *TAG = "es8311";

// The register map, from the datasheet's register section.
constexpr uint8_t kRegReset = 0x00;
constexpr uint8_t kRegClkManager01 = 0x01;
constexpr uint8_t kRegClkManager02 = 0x02;
constexpr uint8_t kRegClkManager03 = 0x03;
constexpr uint8_t kRegClkManager04 = 0x04;
constexpr uint8_t kRegClkManager05 = 0x05;
constexpr uint8_t kRegClkManager06 = 0x06;
constexpr uint8_t kRegClkManager07 = 0x07;
constexpr uint8_t kRegClkManager08 = 0x08;
constexpr uint8_t kRegSdpIn09 = 0x09;
constexpr uint8_t kRegSdpOut0A = 0x0A;
constexpr uint8_t kRegSystem0D = 0x0D;
constexpr uint8_t kRegSystem0E = 0x0E;
constexpr uint8_t kRegSystem12 = 0x12;
constexpr uint8_t kRegSystem13 = 0x13;
constexpr uint8_t kRegAdc1C = 0x1C;
constexpr uint8_t kRegDacMute31 = 0x31;
constexpr uint8_t kRegDacVolume32 = 0x32;
constexpr uint8_t kRegDac37 = 0x37;
constexpr uint8_t kRegChipId1 = 0xFD;  // reads 0x83
constexpr uint8_t kRegChipId2 = 0xFE;  // reads 0x11

constexpr uint8_t kChipId1 = 0x83;
constexpr uint8_t kChipId2 = 0x11;

// The one coefficient row: MCLK = 256×fs, single speed, no pre-division and no
// pre-multiplication, ADC and DAC dividers of 1, LRCK = 256 counts, BCLK
// divider 4, OSR 0x10 for both. Identical for 8 k / 16 k / 32 k / 44.1 k / 48 k
// in the reference table, which is what makes one row enough (see the header).
constexpr uint8_t kPreDiv = 1;      // written as pre_div - 1
constexpr uint8_t kPreMulti = 0;    // 0 => 1x
constexpr uint8_t kAdcDiv = 1;      // written as adc_div - 1
constexpr uint8_t kDacDiv = 1;
constexpr uint8_t kFsMode = 0;      // single speed
constexpr uint8_t kLrckHigh = 0x00;
constexpr uint8_t kLrckLow = 0xFF;  // 0x00ff + 1 = 256 BCLKs per LRCK period
constexpr uint8_t kBclkDiv = 4;     // written as bclk_div - 1 below 19
constexpr uint8_t kAdcOsr = 0x10;
constexpr uint8_t kDacOsr = 0x10;

// Reg 0x09/0x0A, bits 4:2 — the resolution field. 16-bit is 3.
constexpr uint8_t kResolution16 = 3 << 2;

// Reg 0x31, the DAC's two mute bits.
constexpr uint8_t kDacMuteBits = 0x60;

bool RateSupported(uint32_t rate) {
    return rate == 8000 || rate == 16000 || rate == 32000 || rate == 44100 || rate == 48000;
}

}  // namespace

esp_err_t Es8311::WriteRegister(i2cbus::Lease &lease, uint8_t reg, uint8_t value) {
    return lease.WriteRegister(address_, reg, value);
}

esp_err_t Es8311::ReadRegister(i2cbus::Lease &lease, uint8_t reg, uint8_t *value) {
    return lease.ReadRegister(address_, reg, value, 1);
}

esp_err_t Es8311::UpdateRegister(i2cbus::Lease &lease, uint8_t reg, uint8_t keep_mask,
                                 uint8_t set_bits) {
    // Read and write under the caller's lease: a read-modify-write split across
    // two acquisitions is the exact sequence §10.14.3 built the lease for.
    uint8_t value = 0;
    const esp_err_t err = lease.ReadRegister(address_, reg, &value, 1);
    if (err != ESP_OK) {
        return err;
    }
    value = static_cast<uint8_t>((value & keep_mask) | set_bits);
    return lease.WriteRegister(address_, reg, value);
}

esp_err_t Es8311::Identify(i2cbus::Lease &lease, uint8_t address) {
    uint8_t id1 = 0;
    uint8_t id2 = 0;
    esp_err_t err = lease.ReadRegister(address, kRegChipId1, &id1, 1);
    if (err != ESP_OK) {
        return err;
    }
    err = lease.ReadRegister(address, kRegChipId2, &id2, 1);
    if (err != ESP_OK) {
        return err;
    }
    if (id1 != kChipId1 || id2 != kChipId2) {
        ESP_LOGW(TAG, "0x%02x answered with id 0x%02x%02x, expected 0x%02x%02x", address, id1, id2,
                 kChipId1, kChipId2);
        return ESP_ERR_NOT_SUPPORTED;
    }
    return ESP_OK;
}

esp_err_t Es8311::ApplySampleRate(i2cbus::Lease &lease, uint32_t rate) {
    // Registers 0x02..0x08, from the single coefficient row. 0x02 and 0x06 keep
    // the bits this driver has no opinion about; the rest are whole writes.
    //
    // Seven registers, three of them read-modify-write, under the caller's one
    // lease — the reason this took a `Lease &` in the first place.
    esp_err_t err = UpdateRegister(lease, kRegClkManager02, 0x07,
                                   static_cast<uint8_t>(((kPreDiv - 1) << 5) | (kPreMulti << 3)));
    if (err != ESP_OK) {
        return err;
    }
    err = WriteRegister(lease, kRegClkManager03, static_cast<uint8_t>((kFsMode << 6) | kAdcOsr));
    if (err != ESP_OK) {
        return err;
    }
    err = WriteRegister(lease, kRegClkManager04, kDacOsr);
    if (err != ESP_OK) {
        return err;
    }
    err = WriteRegister(lease, kRegClkManager05,
                        static_cast<uint8_t>(((kAdcDiv - 1) << 4) | (kDacDiv - 1)));
    if (err != ESP_OK) {
        return err;
    }
    // Below 19 the divider is written one less than it is; at or above, as it
    // is. Ours is 4, and the branch is kept because the rule is the reference
    // driver's, not this value's.
    err = UpdateRegister(lease, kRegClkManager06, 0xE0,
                         static_cast<uint8_t>(kBclkDiv < 19 ? kBclkDiv - 1 : kBclkDiv));
    if (err != ESP_OK) {
        return err;
    }
    err = UpdateRegister(lease, kRegClkManager07, 0xC0, kLrckHigh);
    if (err != ESP_OK) {
        return err;
    }
    err = WriteRegister(lease, kRegClkManager08, kLrckLow);
    if (err != ESP_OK) {
        return err;
    }

    sample_rate_ = rate;
    return ESP_OK;
}

esp_err_t Es8311::ApplyVolume(i2cbus::Lease &lease, uint8_t percent) {
    // Zero is silence, not the quietest step — the reference driver's mapping,
    // and the one that makes `volume 0` mean what it says.
    const uint8_t reg = percent == 0 ? 0 : static_cast<uint8_t>((percent * 256 / 100) - 1);
    const esp_err_t err = WriteRegister(lease, kRegDacVolume32, reg);
    if (err == ESP_OK) {
        volume_ = percent;
    }
    return err;
}

esp_err_t Es8311::ApplyMute(i2cbus::Lease &lease, bool muted) {
    const esp_err_t err = UpdateRegister(lease, kRegDacMute31,
                                         static_cast<uint8_t>(~kDacMuteBits),
                                         muted ? kDacMuteBits : 0);
    if (err == ESP_OK) {
        muted_ = muted;
    }
    return err;
}

esp_err_t Es8311::SetSampleRate(uint32_t rate) {
    if (!RateSupported(rate)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (bus_ == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    auto lease = bus_->Acquire();
    if (!lease) {
        return ESP_ERR_TIMEOUT;
    }
    return ApplySampleRate(lease, rate);
}

esp_err_t Es8311::SetVolume(uint8_t percent) {
    if (percent > 100) {
        percent = 100;
    }
    if (bus_ == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    auto lease = bus_->Acquire();
    if (!lease) {
        return ESP_ERR_TIMEOUT;
    }
    return ApplyVolume(lease, percent);
}

esp_err_t Es8311::Mute(bool muted) {
    if (bus_ == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    auto lease = bus_->Acquire();
    if (!lease) {
        return ESP_ERR_TIMEOUT;
    }
    return ApplyMute(lease, muted);
}

esp_err_t Es8311::Init(i2cbus::Bus &bus, uint32_t sample_rate) {
    bus_ = &bus;
    present_ = false;

    if (!RateSupported(sample_rate)) {
        return ESP_ERR_INVALID_ARG;
    }

    // **Three leases, and the boundaries are the two `vTaskDelay`s** — not one
    // lease for the whole of Init, which would be simpler to read and would
    // break §10.14.3's other rule: nothing sleeps while holding the bus. A
    // twenty-millisecond nap with the wire held is a touch read dropped and a
    // clock skipped for no reason. So the sequence is cut exactly where it
    // sleeps, and every uninterrupted stretch is one lease.

    // 1. Which address answers, and is it an ES8311.
    {
        auto lease = bus.Acquire();
        if (!lease) {
            return ESP_ERR_TIMEOUT;
        }
        esp_err_t err = Identify(lease, kCodecAddress);
        if (err == ESP_OK) {
            address_ = kCodecAddress;
        } else {
            const esp_err_t first = err;
            err = Identify(lease, kCodecAddressCeHigh);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "no ES8311: 0x%02x %s, 0x%02x %s", kCodecAddress,
                         esp_err_to_name(first), kCodecAddressCeHigh, esp_err_to_name(err));
                return err;
            }
            address_ = kCodecAddressCeHigh;
        }

        // The reset dance is the reference driver's, including the 20 ms: 0x1F
        // puts every block in reset, 0x00 releases them, 0x80 starts the chip's
        // state machine. The lease ends here because the delay does not belong
        // inside it.
        err = WriteRegister(lease, kRegReset, 0x1F);
        if (err != ESP_OK) {
            return err;
        }
    }

    vTaskDelay(pdMS_TO_TICKS(20));

    // 2. Everything else, in one go: release, clocks, format, power-up, volume
    // and mute. About two dozen registers and no sleeping.
    {
        auto lease = bus.Acquire();
        if (!lease) {
            return ESP_ERR_TIMEOUT;
        }

        esp_err_t err = WriteRegister(lease, kRegReset, 0x00);
        if (err != ESP_OK) {
            return err;
        }
        err = WriteRegister(lease, kRegReset, 0x80);
        if (err != ESP_OK) {
            return err;
        }

        // Clock source: MCLK from the MCLK pin (this board wires one), not
        // derived from BCLK, and no inversion. 0x3F enables all the internal
        // clocks.
        err = WriteRegister(lease, kRegClkManager01, 0x3F);
        if (err != ESP_OK) {
            return err;
        }

        err = ApplySampleRate(lease, sample_rate);
        if (err != ESP_OK) {
            return err;
        }

        // Slave mode — the ESP's I²S is the master (bit 6 of the reset register
        // clear) — and 16-bit samples on both serial ports.
        err = UpdateRegister(lease, kRegReset, 0xBF, 0x00);
        if (err != ESP_OK) {
            return err;
        }
        err = WriteRegister(lease, kRegSdpIn09, kResolution16);
        if (err != ESP_OK) {
            return err;
        }
        err = WriteRegister(lease, kRegSdpOut0A, kResolution16);
        if (err != ESP_OK) {
            return err;
        }

        // Power up: analogue on, the DAC out of standby, the output driver on.
        // 0x0E's 0x02 is the reference sequence's value; the microphone path is
        // deliberately left where reset put it (§10.13 — no recording here).
        const uint8_t kPowerUp[][2] = {
            {kRegSystem0D, 0x01}, {kRegSystem0E, 0x02}, {kRegSystem12, 0x00},
            {kRegSystem13, 0x10}, {kRegAdc1C, 0x6A},    {kRegDac37, 0x08},
        };
        for (const auto &write : kPowerUp) {
            err = WriteRegister(lease, write[0], write[1]);
            if (err != ESP_OK) {
                return err;
            }
        }

        // Muted, at a volume that is set but not audible until something
        // unmutes. Through the `Apply` helpers rather than the public setters:
        // those take the bus, and the mutex is not recursive — calling one here
        // would be a deadlock, not just churn.
        err = ApplyVolume(lease, 80);
        if (err != ESP_OK) {
            return err;
        }
        err = ApplyMute(lease, true);
        if (err != ESP_OK) {
            return err;
        }
    }

    present_ = true;
    ESP_LOGI(TAG, "ES8311 at 0x%02x, %" PRIu32 " Hz, muted", address_, sample_rate_);
    return ESP_OK;
}

}  // namespace audio
