#include "board.h"

#include "esp_log.h"

namespace board {

namespace {

constexpr const char *TAG = "board";

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

}  // namespace board
