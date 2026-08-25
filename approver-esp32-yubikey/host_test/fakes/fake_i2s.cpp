// The I²S transmit channel, faked (CLAUDE.md §10.11).
//
// It records rather than plays: how the channel was configured, how many times
// it was stopped and restarted, and **the bytes that went through it**. That
// last one is the point — a WAV parser that got the data offset wrong streams
// the header as audio, and on hardware that is a click nobody can attribute.

#include <cstring>

#include "driver/i2s_std.h"
#include "fake_platform.h"

namespace {

// A single non-null token: the driver only ever checks the handle against null
// and hands it back.
i2s_channel_obj_t *const kChannelToken = reinterpret_cast<i2s_channel_obj_t *>(1);

}  // namespace

namespace fake {

void FailNextI2sWrite(esp_err_t err) { P().i2s.next_write_error = err; }

}  // namespace fake

esp_err_t i2s_new_channel(const i2s_chan_config_t *config, i2s_chan_handle_t *tx,
                          i2s_chan_handle_t *rx) {
    fake::Platform::I2s &i2s = fake::P().i2s;
    if (i2s.channel_open) {
        return ESP_ERR_INVALID_STATE;
    }
    i2s.channel_open = true;
    i2s.auto_clear = config->auto_clear;
    if (tx != nullptr) {
        *tx = kChannelToken;
    }
    if (rx != nullptr) {
        *rx = nullptr;
    }
    return ESP_OK;
}

esp_err_t i2s_del_channel(i2s_chan_handle_t handle) {
    if (handle != kChannelToken) {
        return ESP_ERR_INVALID_ARG;
    }
    fake::P().i2s.channel_open = false;
    return ESP_OK;
}

esp_err_t i2s_channel_init_std_mode(i2s_chan_handle_t handle, const i2s_std_config_t *config) {
    if (handle != kChannelToken) {
        return ESP_ERR_INVALID_ARG;
    }
    fake::Platform::I2s &i2s = fake::P().i2s;
    ++i2s.configure_count;
    i2s.sample_rate = config->clk_cfg.sample_rate_hz;
    i2s.mclk_multiple = static_cast<uint32_t>(config->clk_cfg.mclk_multiple);
    i2s.bits = config->slot_cfg.data_bit_width;
    i2s.slots = config->slot_cfg.slot_mode;
    i2s.mclk = config->gpio_cfg.mclk;
    i2s.bclk = config->gpio_cfg.bclk;
    i2s.ws = config->gpio_cfg.ws;
    i2s.dout = config->gpio_cfg.dout;
    i2s.din = config->gpio_cfg.din;
    return ESP_OK;
}

esp_err_t i2s_channel_reconfig_std_clock(i2s_chan_handle_t handle,
                                         const i2s_std_clk_config_t *config) {
    if (handle != kChannelToken) {
        return ESP_ERR_INVALID_ARG;
    }
    fake::Platform::I2s &i2s = fake::P().i2s;
    // **The real driver refuses this while the channel is running**, which is
    // why `Speaker::Reconfigure` disables it first. Modelled, so a driver that
    // stopped doing that would fail here rather than on the bench.
    if (i2s.enabled) {
        return ESP_ERR_INVALID_STATE;
    }
    ++i2s.reconfig_count;
    i2s.sample_rate = config->sample_rate_hz;
    i2s.mclk_multiple = static_cast<uint32_t>(config->mclk_multiple);
    return ESP_OK;
}

esp_err_t i2s_channel_enable(i2s_chan_handle_t handle) {
    if (handle != kChannelToken) {
        return ESP_ERR_INVALID_ARG;
    }
    fake::Platform::I2s &i2s = fake::P().i2s;
    i2s.enabled = true;
    ++i2s.enable_count;
    return ESP_OK;
}

esp_err_t i2s_channel_disable(i2s_chan_handle_t handle) {
    if (handle != kChannelToken) {
        return ESP_ERR_INVALID_ARG;
    }
    fake::Platform::I2s &i2s = fake::P().i2s;
    i2s.enabled = false;
    ++i2s.disable_count;
    return ESP_OK;
}

esp_err_t i2s_channel_write(i2s_chan_handle_t handle, const void *src, size_t size,
                            size_t *written, uint32_t timeout_ms) {
    (void)timeout_ms;
    if (handle != kChannelToken) {
        return ESP_ERR_INVALID_ARG;
    }
    fake::Platform::I2s &i2s = fake::P().i2s;
    if (!i2s.enabled) {
        return ESP_ERR_INVALID_STATE;
    }

    ++i2s.write_calls;
    if (i2s.next_write_error != ESP_OK) {
        const esp_err_t err = i2s.next_write_error;
        i2s.next_write_error = ESP_OK;
        if (written != nullptr) {
            *written = 0;
        }
        return err;
    }

    const size_t room = fake::Platform::I2s::kMaxCaptured - i2s.captured_length;
    const size_t copy = size < room ? size : room;
    if (copy < size) {
        i2s.captured_overflowed = true;
    }
    std::memcpy(i2s.captured + i2s.captured_length, src, copy);
    i2s.captured_length += copy;
    i2s.written_total += size;

    if (written != nullptr) {
        *written = size;
    }
    return ESP_OK;
}
