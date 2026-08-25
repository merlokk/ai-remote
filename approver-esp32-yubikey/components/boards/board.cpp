// The board, brought up (CLAUDE.md §10.1). Short, because there is not much on
// it: one button, one serial LED, and two USB sockets neither of which needs a
// line of setup here.

#include "board.h"

#include "esp_log.h"
#include "led.h"

namespace board {
namespace {

constexpr const char *TAG = "board";

buttons::Buttons buttons_;
bool buttons_ready = false;
bool ready = false;

// **`BOOT` shorts its pin to ground: idle high, pressed 0** — and the internal
// pull-up is enabled because GPIO0 has an external one on this board but a
// firmware that relies on a board's pull-up reads a floating pin as a stuck
// button the day it is used on a revision that dropped it.
constexpr buttons::Config kButtons[] = {
    {button::kBoot, "BOOT", true, true},
};

}  // namespace

void LogPinout() {
    ESP_LOGI(TAG, "%s", kName);
    ESP_LOGI(TAG, "  BOOT      GPIO%d", static_cast<int>(button::kBoot));
    ESP_LOGI(TAG, "  WS2812    GPIO%d on UART%d", static_cast<int>(led::kData),
             static_cast<int>(led::kUart));
    ESP_LOGI(TAG, "  console   UART%d, TX GPIO%d / RX GPIO%d (CH343P)",
             static_cast<int>(console::kUart), static_cast<int>(console::kTx),
             static_cast<int>(console::kRx));
    ESP_LOGI(TAG, "  USB host  GPIO%d/%d, VBUS not switchable",
             static_cast<int>(usb::kDataMinus), static_cast<int>(usb::kDataPlus));
}

esp_err_t InitButtons() {
    if (buttons_ready) {
        return ESP_OK;
    }
    const esp_err_t err = buttons_.Init(kButtons, button::kCount);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "buttons: %s", esp_err_to_name(err));
        return err;
    }
    buttons_ready = true;
    return ESP_OK;
}

esp_err_t Init() {
    if (ready) {
        return ESP_OK;
    }
    LogPinout();
    InitButtons();

    const esp_err_t err = ::led::Init(led::kUart, led::kData);
    if (err != ESP_OK) {
        // §10.10: reported, not fatal. The device can still take a request and
        // still sign a verdict; what it cannot do is say so.
        ESP_LOGE(TAG, "the LED did not come up (%s); this device will be silent",
                 esp_err_to_name(err));
    }

    ready = true;
    return ESP_OK;
}

buttons::Buttons &Buttons() { return buttons_; }

bool BootPressed() {
    if (!buttons_ready) {
        return false;
    }
    return buttons_.RawPressed(button::kBootIndex);
}

}  // namespace board
