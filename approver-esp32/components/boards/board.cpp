#include "board.h"

#include "esp_log.h"

namespace board {

namespace {

constexpr const char *TAG = "board";

// The devices, by value, constructed in declaration order and living for the
// life of the device (§10.14.1). Their constructors are trivial: nothing here
// touches a wire until Init() runs, from app_main.
i2cbus::Bus bus;
pmic::Axp2101 axp;

}  // namespace

void LogPinout() {
    ESP_LOGI(TAG, "%s, %dx%d", kName, kScreenWidth, kScreenHeight);
    ESP_LOGI(TAG, "  i2c      scl=%d sda=%d", i2c::kScl, i2c::kSda);
    ESP_LOGI(TAG, "  buttons  boot=%d key=%d pwr=%d", button::kBoot, button::kKey,
             button::kPwr);
    ESP_LOGI(TAG, "  display  cs=%d sclk=%d d0=%d d1=%d d2=%d d3=%d, reset on PMIC ALDO3",
             display::kChipSelect, display::kSclk, display::kData0, display::kData1,
             display::kData2, display::kData3);
    ESP_LOGI(TAG, "  touch    rst=%d int=%d", touch::kReset, touch::kInterrupt);
    ESP_LOGI(TAG, "  imu      int1=%d int2=%d", imu::kInterrupt1, imu::kInterrupt2);
    ESP_LOGI(TAG, "  audio    mclk=%d sclk=%d asdout=%d lrck=%d dsdin=%d, PA on PMIC ALDO2",
             audio::kMclk, audio::kSclk, audio::kAsdout, audio::kLrck, audio::kDsdin);
    ESP_LOGI(TAG, "  tf       sck=%d mosi=%d miso=%d cs=%d (shares the panel's QSPI)",
             sdcard::kSck, sdcard::kMosi, sdcard::kMiso, sdcard::kChipSelect);
}

i2cbus::Bus &I2c() { return bus; }

pmic::Axp2101 &Pmic() { return axp; }

esp_err_t Init() {
    // The bus first: everything below it is on it. A failure here is fatal to
    // the whole I²C half of the board, so it returns rather than continuing to
    // ask chips that cannot answer.
    esp_err_t err = bus.Init(i2c::kScl, i2c::kSda);
    if (err != ESP_OK) {
        return err;
    }

    // The PMIC next, and it is deliberately first among the chips: §10.1 —
    // the panel's reset and the amplifier's enable are its rails, so nothing
    // else on this board comes up before it does.
    err = axp.Init(bus);
    if (err != ESP_OK) {
        // Not fatal here. The device still boots, the console still answers,
        // and `power` says the chip did not respond — which is more useful
        // than a boot loop (§10.10's rule about staying up to report).
        ESP_LOGE(TAG, "PMIC not initialised: %s", esp_err_to_name(err));
    }

    return ESP_OK;
}

}  // namespace board
