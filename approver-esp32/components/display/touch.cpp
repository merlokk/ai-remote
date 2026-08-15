#include "touch.h"

#include "esp_lcd_touch_cst9217.h"
#include "esp_log.h"

namespace display {

namespace {

constexpr const char *TAG = "touch";

// Long enough that a touch read waits out an ordinary PMIC or RTC transaction,
// short enough that a wedged bus costs one frame rather than the frame rate.
// §10.14.3's rule, with a number: nothing here blocks.
constexpr uint32_t kLeaseTimeoutMs = 20;

}  // namespace

esp_err_t Touch::Init(i2cbus::Bus &bus, const TouchConfig &config) {
    if (handle_ != nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!bus.Ready()) {
        return ESP_ERR_INVALID_STATE;
    }
    bus_ = &bus;

    // **Field by field rather than through the driver's own macro**, and this
    // is a build rule rather than taste: the project compiles with
    // `-Werror=missing-field-initializers`, and a brace initialiser from a
    // header that names five of a dozen members fails it. Zero-initialising and
    // assigning is what the rest of this repository does with IDF configs for
    // the same reason. The values are the macro's.
    esp_lcd_panel_io_i2c_config_t io_config = {};
    io_config.dev_addr = ESP_LCD_TOUCH_IO_I2C_CST9217_ADDRESS;
    io_config.scl_speed_hz = 400000;
    io_config.control_phase_bytes = 1;
    io_config.dc_bit_offset = 0;
    io_config.lcd_cmd_bits = 8;
    io_config.flags.disable_control_phase = 1;

    esp_lcd_panel_io_handle_t io = nullptr;
    // **The one place the raw bus handle is used.** The driver has to own its
    // own device on this bus; what it does not get is permission to skip the
    // lease, and `Read` below is where that permission is withheld.
    esp_err_t err = esp_lcd_new_panel_io_i2c(bus.Handle(), &io_config, &io);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "touch IO not created: %s", esp_err_to_name(err));
        return err;
    }

    esp_lcd_touch_config_t touch_config = {};
    touch_config.x_max = static_cast<uint16_t>(config.width);
    touch_config.y_max = static_cast<uint16_t>(config.height);
    touch_config.rst_gpio_num = config.reset;
    touch_config.int_gpio_num = config.interrupt;
    touch_config.levels.reset = 0;
    touch_config.levels.interrupt = 0;
    // The orientation, and the only three numbers here that are not obvious —
    // `touch.h` says where they come from.
    touch_config.flags.swap_xy = config.swap_xy;
    touch_config.flags.mirror_x = config.mirror_x;
    touch_config.flags.mirror_y = config.mirror_y;

    // The controller is reset over its own line here, which is why this runs
    // outside a lease: the driver's probe is several transfers and takes the
    // bus for as long as it needs it, at boot, with nothing else running.
    err = esp_lcd_touch_new_i2c_cst9217(io, &touch_config, &handle_);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "CST9220 did not answer: %s", esp_err_to_name(err));
        handle_ = nullptr;
        return err;
    }

    ESP_LOGI(TAG, "CST9220 up: %dx%d, swap_xy=%d mirror_x=%d mirror_y=%d", config.width,
             config.height, config.swap_xy, config.mirror_x, config.mirror_y);
    return ESP_OK;
}

bool Touch::Read(uint16_t *x, uint16_t *y) {
    if (handle_ == nullptr || bus_ == nullptr) {
        return false;
    }

    auto lease = bus_->Acquire(kLeaseTimeoutMs);
    if (!lease) {
        // Not an error and not worth a log line per frame — a counter the
        // console can read instead (§10.14.3: the caller decides what a miss
        // means, and for touch it means this frame has no touch).
        ++missed_;
        return false;
    }

    if (esp_lcd_touch_read_data(handle_) != ESP_OK) {
        return false;
    }

    // One point. This device has two buttons on a card and no gesture that
    // means anything (§10.13) — a second finger is a second finger, not a
    // second verdict.
    esp_lcd_touch_point_data_t point = {};
    uint8_t count = 0;
    if (esp_lcd_touch_get_data(handle_, &point, &count, 1) != ESP_OK || count == 0) {
        return false;
    }

    *x = point.x;
    *y = point.y;
    return true;
}

}  // namespace display
