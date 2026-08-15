#pragma once

// The panel — a CO5300 AMOLED, 480x480, over QSPI (CLAUDE.md §10.1).
//
// Library layer: it knows about wires and pixels and nothing about approvals
// (§10.14.2). What comes out of it is an `esp_lcd` panel handle and the IO
// handle behind it; who draws what on it is the logic layer's business, and
// this file must never learn the word "screen".
//
// **The driver is `espressif/esp_lcd_sh8601` and the glass is a CO5300**, which
// is not a mistake and is worth the paragraph. There is no CO5300 component on
// the registry, and the vendor's own ESP-IDF example for this board
// (`02_Example/ESP-IDF-v5.5.3/09_LVGL_V9_Test`) drives it with the SH8601 one:
// the two speak the same QSPI framing, and everything specific to this glass
// arrives as the init list in `panel.cpp` rather than out of the driver. §10.4
// says "CO5300 panel driver"; this is what that turned out to mean.
//
// **The panel's reset is not a GPIO** — it is the PMIC's ALDO3 rail (§10.1). A
// display driver that knew what an AXP2101 was would be the layering mistake
// §10.14.2 exists to prevent, so the rail arrives as a plain function pointer
// (§10.14.1 — no `std::function`): `components/boards` knows the rail belongs
// to a PMIC, this file knows only that something has to go off and on again.

#include <cstddef>
#include <cstdint>

#include "driver/spi_master.h"
#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "hal/gpio_types.h"

namespace display {

inline constexpr int kWidth = 480;
inline constexpr int kHeight = 480;

// The vendor's clock for this glass. Faster has not been tried; the flush is
// not what limits this device.
inline constexpr uint32_t kDefaultPclkHz = 40 * 1000 * 1000;

// **The rows one flush carries, and the number §10.1 argues for.** There is no
// PSRAM on the C6 and no way to add it, so a full 480x480x16bpp framebuffer —
// 460 800 bytes against 512 KB shared with lwIP, Wi-Fi and TLS — is not on the
// table. Two buffers of 480x40 are 38.4 KB each, and the panel is fed by DMA
// per flush. Any vendor demo that claims full double buffering on this chip is
// doing it with partial buffers too.
inline constexpr int kFlushLines = 40;

struct Config {
    // The QSPI wires. Arguments rather than an include of `board.h`: this layer
    // knows about wires, not about which board they are on (§10.14.2) — the
    // same rule `i2cbus::Bus::Init` follows.
    gpio_num_t sclk = GPIO_NUM_NC;
    gpio_num_t data0 = GPIO_NUM_NC;
    gpio_num_t data1 = GPIO_NUM_NC;
    gpio_num_t data2 = GPIO_NUM_NC;
    gpio_num_t data3 = GPIO_NUM_NC;
    gpio_num_t chip_select = GPIO_NUM_NC;

    int width = kWidth;
    int height = kHeight;
    int flush_lines = kFlushLines;

    spi_host_device_t host = SPI2_HOST;
    uint32_t pclk_hz = kDefaultPclkHz;

    // The panel's reset, which on this board is a power rail rather than a pin.
    // Called with `false` then `true` around the init sequence; a null pointer
    // means the panel is assumed to be already powered, which is honest for a
    // board where it is not a rail at all.
    void (*power)(bool on, void *context) = nullptr;
    void *power_context = nullptr;
};

class Panel {
   public:
    Panel() = default;
    Panel(const Panel &) = delete;
    Panel &operator=(const Panel &) = delete;

    // Trivial constructor, separate Init (§10.14.1). This one is slow on
    // purpose — the reset sequence and the panel's own sleep-out take about
    // three quarters of a second, and shortening either produces a display that
    // works on most boots.
    esp_err_t Init(const Config &config);
    bool Ready() const { return panel_ != nullptr; }

    // 0..100. On an AMOLED this is a panel command, not a backlight pin — there
    // is no backlight on this board to switch (§10.1 records the sheet has no
    // line for one, and this is why).
    esp_err_t SetBrightness(uint8_t percent);
    uint8_t Brightness() const { return brightness_; }

    // The blank of §10.8.1's idle timeout. Separate from brightness because
    // they are different states: a display at 0 % is still refreshing.
    esp_err_t SetOn(bool on);
    bool On() const { return on_; }

    esp_lcd_panel_handle_t Handle() const { return panel_; }
    esp_lcd_panel_io_handle_t Io() const { return io_; }

    int Width() const { return config_.width; }
    int Height() const { return config_.height; }

    // What one LVGL draw buffer has to hold, in pixels. The port allocates them
    // (§10.14.1: the libraries allocate, our code does not), and this is the
    // number it is told.
    size_t FlushBufferPixels() const {
        return static_cast<size_t>(config_.width) * static_cast<size_t>(config_.flush_lines);
    }

   private:
    esp_err_t Reset();
    esp_err_t TxParam(uint8_t command, const uint8_t *data, size_t length);

    Config config_ = {};
    esp_lcd_panel_handle_t panel_ = nullptr;
    esp_lcd_panel_io_handle_t io_ = nullptr;
    uint8_t brightness_ = 100;
    bool on_ = false;
};

}  // namespace display
