#include "board.h"

#include <sys/time.h>

#include <ctime>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace board {

namespace {

constexpr const char *TAG = "board";

// The devices, by value, constructed in declaration order and living for the
// life of the device (§10.14.1). Their constructors are trivial: nothing here
// touches a wire until Init() runs, from app_main.
i2cbus::Bus bus;
pmic::Axp2101 axp;
rtc::Pcf85063 clock_chip;
::imu::Qmi8658 motion;
::audio::Es8311 codec;
::audio::Speaker sound;
buttons::Buttons keys;

// The rate the sounds in `spiffs_image/` are converted to. One number, in one
// place: the codec is configured for it at boot and `Speaker` retunes both
// halves if a file turns out to be something else.
constexpr uint32_t kAudioSampleRate = 16000;

// The pin map turned into the driver's table. The order is `button::Index`'s,
// and the static_assert below is what keeps the two from drifting.
//
// **`pwr` is the odd one, and the polarity was measured rather than assumed.**
// `BOOT` and `KEY` short their pin to ground: idle high, pressed 0. GPIO18 does
// the opposite — it reads **0 at rest with the internal pull-up on** (so the
// line is driven, not floating) and goes high while the button is held. That is
// the inverse of the AXP2101's PWRON pin, which §10.1 correctly describes as
// pressed = 0: the chip sees the button, and the ESP sees it through something
// that inverts. Source: this board, `buttons watch`, one press.
constexpr buttons::Config kButtonConfigs[] = {
    {.pin = button::kBoot, .name = "boot", .active_low = true, .pull_up = true},
    {.pin = button::kKey, .name = "key", .active_low = true, .pull_up = true},
    {.pin = button::kPwr, .name = "pwr", .active_low = false, .pull_up = false},
};
static_assert(sizeof(kButtonConfigs) / sizeof(kButtonConfigs[0]) == button::kCount,
              "the table and button::Index disagree about how many buttons there are");
static_assert(kButtonConfigs[button::kKeyIndex].pin == button::kKey,
              "button::Index no longer names the right row — §10.15 reads kKeyIndex");

// §10.8.2: the RTC is the time source at boot — instant, offline, and correct
// across a power cut. SNTP corrects it later, when there is a network. A clock
// the chip says it cannot vouch for is left alone: an unset system clock is
// honest, a plausible wrong one is not.
void AdoptClock() {
    rtc::DateTime now = {};
    bool valid = false;
    if (clock_chip.Read(&now, &valid) != ESP_OK) {
        return;
    }
    if (!valid) {
        ESP_LOGW(TAG, "RTC has no trustworthy time; system clock left unset");
        return;
    }

    struct tm fields = {};
    fields.tm_year = now.year - 1900;
    fields.tm_mon = now.month - 1;
    fields.tm_mday = now.day;
    fields.tm_hour = now.hour;
    fields.tm_min = now.minute;
    fields.tm_sec = now.second;
    fields.tm_isdst = 0;

    // No timezone anywhere yet: the RTC holds what was written to it and this
    // treats it as the system clock's own reading. §10.8.2 owns the TZ string
    // when there is a settings screen to hold it.
    const time_t seconds = mktime(&fields);
    if (seconds <= 0) {
        return;
    }
    const timeval tv = {.tv_sec = seconds, .tv_usec = 0};
    settimeofday(&tv, nullptr);
    ESP_LOGI(TAG, "system clock set from RTC: %04u-%02u-%02u %02u:%02u:%02u", now.year,
             now.month, now.day, now.hour, now.minute, now.second);
}

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

rtc::Pcf85063 &Clock() { return clock_chip; }

buttons::Buttons &Buttons() { return keys; }

::imu::Qmi8658 &Imu() { return motion; }

::audio::Es8311 &Codec() { return codec; }

::audio::Speaker &Sound() { return sound; }

bool ImuInterrupt1() { return gpio_get_level(imu::kInterrupt1) != 0; }

bool ImuInterrupt2() { return gpio_get_level(imu::kInterrupt2) != 0; }

esp_err_t Init() {
    // The buttons before the bus, and deliberately: they depend on nothing, and
    // §10.15's restore is a `KEY` read that has to happen before the config is
    // parsed — so a bus that fails must not be able to take them with it.
    esp_err_t button_err = keys.Init(kButtonConfigs, button::kCount);
    if (button_err != ESP_OK) {
        ESP_LOGE(TAG, "buttons not initialised: %s", esp_err_to_name(button_err));
    }

    // The bus next: everything below it is on it. A failure here is fatal to
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

    // The RTC after the PMIC, which backs it (§10.1).
    err = clock_chip.Init(bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "RTC not initialised: %s", esp_err_to_name(err));
    } else {
        AdoptClock();
    }

    // The IMU last, because it is the only chip here nothing depends on
    // (§10.13). A failure is a console readout that says so, and no more.
    err = motion.Init(bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "IMU not initialised: %s", esp_err_to_name(err));
    }

    // Its two interrupt lines as inputs, and that is all: no ISR, no handler,
    // nothing subscribed. The pull-down makes an undriven line read a steady
    // low, so "the chip is not using this pin" and "the pin is floating" do not
    // look the same on the console (§10.13's readout, not a feature).
    // Audio, and the order inside it is a hardware fact rather than taste
    // (§10.1): the amplifier's enable is the PMIC's ALDO2, so the rail comes
    // up first and the codec is talked to second. It is left **on** while the
    // board is on and the codec stays muted between sounds — the alternative,
    // switching the rail per chirp, trades hiss for a pop and a settling delay
    // in front of every sound.
    if (axp.Present()) {
        const esp_err_t rail = axp.SetAldo2(true);
        if (rail != ESP_OK) {
            ESP_LOGE(TAG, "audio rail (ALDO2) not enabled: %s", esp_err_to_name(rail));
        } else {
            vTaskDelay(pdMS_TO_TICKS(20));  // let the rail settle before I²C to the codec
        }
    }

    err = codec.Init(bus, kAudioSampleRate);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "codec not initialised: %s", esp_err_to_name(err));
    } else {
        const ::audio::SpeakerPins pins = {
            .mclk = audio::kMclk,
            .bclk = audio::kSclk,
            .lrck = audio::kLrck,
            .data_out = audio::kDsdin,
        };
        err = sound.Init(codec, pins, kAudioSampleRate);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "speaker not initialised: %s", esp_err_to_name(err));
        }
    }

    gpio_config_t interrupt_pins = {};
    interrupt_pins.pin_bit_mask = (1ULL << static_cast<uint32_t>(imu::kInterrupt1)) |
                                  (1ULL << static_cast<uint32_t>(imu::kInterrupt2));
    interrupt_pins.mode = GPIO_MODE_INPUT;
    interrupt_pins.pull_up_en = GPIO_PULLUP_DISABLE;
    interrupt_pins.pull_down_en = GPIO_PULLDOWN_ENABLE;
    interrupt_pins.intr_type = GPIO_INTR_DISABLE;
    if (gpio_config(&interrupt_pins) != ESP_OK) {
        ESP_LOGW(TAG, "IMU interrupt pins not configured as inputs");
    }

    return ESP_OK;
}

}  // namespace board
