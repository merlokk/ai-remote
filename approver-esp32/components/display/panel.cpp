#include "panel.h"

#include <cinttypes>

#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_sh8601.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace display {

namespace {

constexpr const char *TAG = "display";

// **The init sequence for this glass, and its source is the whole point.**
// Copied from the vendor's own example for this exact board —
// `02_Example/ESP-IDF-v5.5.3/09_LVGL_V9_Test/components/port_bsp/display_bsp.cpp`
// — because a panel init list is not something to derive from a datasheet and
// hope: the CO5300 datasheet in `docs/` documents the commands, and only the
// board vendor knows which gamma and porch values this module was tuned with.
//
// The ones worth being able to read back, since three of them are decisions
// rather than magic:
//
//   0x11  sleep out, and **600 ms** afterwards. The panel does not answer
//         before that and the delay is not padding.
//   0xFE  page select — 0x20 opens the manufacturer page for the two commands
//         after it, 0x00 returns to the user page. Leaving the page open makes
//         every later command land somewhere unintended.
//   0x3A  0x55 = 16 bits per pixel, which is what `bits_per_pixel` below and
//         LVGL's RGB565 have to agree with.
//   0x36  0x30 is this module's scan order. `Panel::SetRotate` writes the same
//         register, which is why rotation is a command and not a redraw.
//   0x51  brightness, full. `SetBrightness` writes this one at runtime.
//   0x2A/0x2B  the column and row address windows: 0x0000..0x01DF is 0..479.
//   0x29  display on, and 100 ms after it.
//
// **Two warnings at boot belong to this list and are correct**, so that nobody
// spends an evening on them: the driver logs
//
//   W sh8601: The 3Ah command has been used and will be overwritten by
//             external initialization sequence
//
// and the same for 36h. It is saying that our list sets the pixel format and
// the scan order itself instead of taking the driver's defaults, which is
// exactly what a vendor init list is for.
constexpr sh8601_lcd_init_cmd_t kInitCommands[] = {
    {0x11, (uint8_t[]){0x00}, 0, 600},
    {0xFE, (uint8_t[]){0x20}, 1, 0},
    {0x19, (uint8_t[]){0x10}, 1, 0},
    {0x1C, (uint8_t[]){0xA0}, 1, 0},
    {0xFE, (uint8_t[]){0x00}, 1, 0},
    {0xC4, (uint8_t[]){0x80}, 1, 0},
    {0x3A, (uint8_t[]){0x55}, 1, 0},
    {0x35, (uint8_t[]){0x00}, 1, 0},
    {0x36, (uint8_t[]){0x30}, 1, 0},
    {0x53, (uint8_t[]){0x20}, 1, 0},
    {0x51, (uint8_t[]){0xFF}, 1, 0},
    {0x63, (uint8_t[]){0xFF}, 1, 0},
    {0x2A, (uint8_t[]){0x00, 0x00, 0x01, 0xDF}, 4, 0},
    {0x2B, (uint8_t[]){0x00, 0x00, 0x01, 0xDF}, 4, 0},
    {0x29, (uint8_t[]){0x00}, 0, 100},
};

constexpr uint8_t kCommandBrightness = 0x51;

// **A command sent by hand needs the QSPI framing the driver applies to its
// own.** On this transport a parameter write goes out as `0x02 << 24 | cmd <<
// 8`: the leading 0x02 is the write opcode and the command sits in the second
// byte. Sending the bare command number is the mistake that produces a panel
// that ignores brightness while everything else works.
constexpr uint32_t Framed(uint8_t command) {
    return (static_cast<uint32_t>(0x02) << 24) | (static_cast<uint32_t>(command) << 8);
}

}  // namespace

esp_err_t Panel::Init(const Config &config) {
    if (panel_ != nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    config_ = config;

    // One flush has to fit in one transfer, so the SPI bus is sized off the
    // same number the draw buffers are (§10.1). Asking for more would reserve
    // DMA descriptors nothing uses.
    const int max_transfer = config_.width * config_.flush_lines * 2;

    spi_bus_config_t bus = {};
    bus.sclk_io_num = config_.sclk;
    bus.data0_io_num = config_.data0;
    bus.data1_io_num = config_.data1;
    bus.data2_io_num = config_.data2;
    bus.data3_io_num = config_.data3;
    bus.max_transfer_sz = max_transfer;

    esp_err_t err = spi_bus_initialize(config_.host, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPI bus not initialised: %s", esp_err_to_name(err));
        return err;
    }

    esp_lcd_panel_io_spi_config_t io = {};
    io.cs_gpio_num = config_.chip_select;
    // **No D/C pin, and none is missing.** QSPI carries the command/data
    // distinction in the framing above rather than on a wire, which is also why
    // `board.h` has no line for one.
    io.dc_gpio_num = GPIO_NUM_NC;
    io.spi_mode = 0;
    io.pclk_hz = config_.pclk_hz;
    io.trans_queue_depth = 1;
    io.lcd_cmd_bits = 32;
    io.lcd_param_bits = 8;
    io.flags.quad_mode = true;

    err = esp_lcd_new_panel_io_spi(static_cast<esp_lcd_spi_bus_handle_t>(config_.host), &io, &io_);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "panel IO not created: %s", esp_err_to_name(err));
        return err;
    }

    sh8601_vendor_config_t vendor = {};
    vendor.init_cmds = kInitCommands;
    vendor.init_cmds_size = sizeof(kInitCommands) / sizeof(kInitCommands[0]);
    vendor.flags.use_qspi_interface = 1;

    esp_lcd_panel_dev_config_t panel = {};
    // Not a pin on this board — see `Reset()`.
    panel.reset_gpio_num = GPIO_NUM_NC;
    panel.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
    panel.bits_per_pixel = 16;
    panel.vendor_config = &vendor;

    err = esp_lcd_new_panel_sh8601(io_, &panel, &panel_);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "panel not created: %s", esp_err_to_name(err));
        panel_ = nullptr;
        return err;
    }

    // The rail, then the init list. In that order: the sequence above starts
    // with sleep-out, and a panel that has not been power-cycled since the last
    // boot answers it from whatever state it was left in.
    err = Reset();
    if (err != ESP_OK) {
        return err;
    }

    err = esp_lcd_panel_init(panel_);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "panel not initialised: %s", esp_err_to_name(err));
        return err;
    }

    on_ = true;
    brightness_ = 100;

    ESP_LOGI(TAG, "CO5300 up: %dx%d, QSPI at %" PRIu32 " MHz, flush %d lines (%d B)",
             config_.width, config_.height, config_.pclk_hz / 1000000, config_.flush_lines,
             max_transfer);
    return ESP_OK;
}

esp_err_t Panel::Reset() {
    if (config_.power == nullptr) {
        // Nothing to switch. Said out loud rather than passed over: on this
        // board it means the caller forgot the rail, and the symptom is a
        // panel that initialises without complaint and stays black.
        ESP_LOGW(TAG, "no power callback — the panel is being initialised without a reset");
        return ESP_OK;
    }

    // On, off, on, with the vendor's 100 ms between each. The leading `on` is
    // deliberate: after a warm reset the rail is already up, and the off is
    // what the panel actually needs to see.
    config_.power(true, config_.power_context);
    vTaskDelay(pdMS_TO_TICKS(100));
    config_.power(false, config_.power_context);
    vTaskDelay(pdMS_TO_TICKS(100));
    config_.power(true, config_.power_context);
    vTaskDelay(pdMS_TO_TICKS(100));
    return ESP_OK;
}

esp_err_t Panel::TxParam(uint8_t command, const uint8_t *data, size_t length) {
    if (io_ == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    return esp_lcd_panel_io_tx_param(io_, Framed(command), data, length);
}

esp_err_t Panel::SetBrightness(uint8_t percent) {
    if (percent > 100) {
        percent = 100;
    }
    const uint8_t level = static_cast<uint8_t>((static_cast<uint16_t>(percent) * 255) / 100);
    const esp_err_t err = TxParam(kCommandBrightness, &level, 1);
    if (err == ESP_OK) {
        brightness_ = percent;
    }
    return err;
}

esp_err_t Panel::SetOn(bool on) {
    if (panel_ == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t err = esp_lcd_panel_disp_on_off(panel_, on);
    if (err == ESP_OK) {
        on_ = on;
    }
    return err;
}

}  // namespace display
