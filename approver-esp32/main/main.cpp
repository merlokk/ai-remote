// The entry point. `main/` stays thin (CLAUDE.md §10.14.2): the library layer
// lives in `components/` and the logic goes on top of it — the layer is written
// and most of the logic is not, so what this file does is **compose**, in an
// order that is written down rather than implied, and hand the result to the one
// screen of §10.8 that exists.
//
// What it composes: the filesystem, the settings on it, the zone, the board and
// its chips, the console, the radio, the clock's network half, the bus — then
// the two settings that have hardware to reach (volume, brightness), then the
// splash and the boot chime, then LVGL and the screens on it. Every step below
// says why it is where it is; the one rule none of them breaks is that a failure
// here is a log line and not a branch (§10.10: a device that cannot mount its
// storage should still come up far enough to say so).
//
// **The placeholder that used to be at the end of it is gone**, which is what
// §10.8 said would happen to it: the clock of §10.8.2 is a real screen and it
// answers both questions the placeholder existed to answer — the colours are
// right if the digits are green, and the layout is right if the whole face
// drifts without leaving the glass.

#include <cinttypes>

#include "board.h"
#include "config.h"
#include "console.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl_display.h"
#include "nats_link.h"
#include "rawimage.h"
#include "screens.h"
#include "storage.h"
#include "timesync.h"
#include "timezone.h"
#include "wifi_manager.h"

namespace {

constexpr const char *TAG = "app";

// Flashed with the image from `spiffs_image/` (§10.15). Uncompressed 16 kHz
// mono PCM, because the firmware has no decoder and does not want one — the
// argument is in `speaker.h`.
constexpr const char *kBootSound = "poweron.wav";

// The boot splash (§10.8): white katakana, Matrix-fashion, generated on the
// host by `tools/make-splash.ps1` and flashed with the SPIFFS image. Raw
// RGB565 in the panel's own byte order — `rawimage.h` argues why the firmware
// has no decoder, and it is `speaker.h`'s argument applied to pixels.
constexpr const char *kSplashImage = "splash.bin";

// **A floor, not a duration, and measured from the start of the blit.** The
// boot sound plays under the splash and takes about three seconds, so that is
// what decides how long the picture is up; this is what keeps a device with no
// codec — or a shorter chime — from flashing the splash and moving on. Timing
// it from the start also stops a slow filesystem from being added to the wait:
// the 460 KB is on the glass while it streams.
constexpr int64_t kSplashMs = 2000;

}  // namespace

extern "C" void app_main(void) {
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_app_desc_t *desc = esp_app_get_description();

    ESP_LOGI(TAG, "%s %s, running from %s at 0x%" PRIx32 " (%" PRIu32 " KB)",
             desc->project_name, desc->version, running->label,
             running->address, running->size / 1024);

    board::LogPinout();

    // Order matters and is written down rather than implied (§10.14.1), and it
    // now reads bottom-up: the filesystem, then the settings on it, then the
    // hardware those settings configure, then the console — so that `power`,
    // `cat` and `play` all have something to answer with the moment the prompt
    // appears. None of these failures is fatal: a device that cannot mount its
    // storage or reach its PMIC should still come up far enough to say so.
    //
    // **The filesystem moved in front of the board on purpose.** It depends on
    // nothing, and the codec's volume comes out of `config.json` (§10.15) —
    // bringing the hardware up first would mean setting that volume twice, and
    // the second write would be the one that mattered.
    storage::Init();
    config::Init();

    // The zone before the clock: `board::Init` adopts the RTC and logs what it
    // found, and a log line in the wrong zone is a bug report about the RTC.
    // Nothing here moves a stored value — the RTC and `time_t` are UTC, and
    // this only decides how they are read back (§10.8.2).
    tz::Apply(config::Get().time.posix);

    board::Init();
    console::Init();

    // The radio (§10.9), after the settings it reads and after the console
    // that can fix them. It starts a task and **not** the radio: what happens
    // next is whatever `config.json` asks for, and the shipped file asks for
    // nothing — `esp_wifi_init` costs tens of kilobytes of heap, and a device
    // configured with Wi-Fi off should not pay them.
    wifimgr::Init();

    // And the clock's network half (§10.8.2), which is why it is after the
    // radio: it has nothing to do until there is an internet, and `wifimgr` is
    // what tells it there is one. It starts a task and asks nothing — a device
    // with no network, or with `time.syncHours` at 0, pays a task and no
    // packets for this existing.
    //
    // The RTC is passed in rather than reached for: `timesync` has never heard
    // of this board (§10.14.2), and `main` is where the two meet — the same
    // shape as the codec's volume below.
    timesync::Init(&board::Clock());

    // And the bus (§10.3), for the same reason in the same place: it has
    // nothing to do until there is a client link with an address, and
    // `wifimgr` is what tells it there is one. A task and no socket — a device
    // with no `nats.url` pays for this existing and nothing more.
    //
    // **Not the clock's question, though.** `timesync` waits for an internet;
    // this waits for a *network*, because the server is on the LAN and a
    // household router with its uplink down is a perfectly good place to
    // approve a command.
    nats::Init();

    // Settings applied to the hardware they belong to. `main` is where the two
    // meet: `config` knows nothing about a codec, and `board` knows nothing
    // about a file (§10.14.2).
    if (board::Codec().Present()) {
        board::Codec().SetVolume(config::Get().audio.volume_percent);
    }

    // **And the brightness, which was stored and never applied.** §10.15 calls
    // the volume "the first setting that round-trips"; this is the second, and
    // until now it only looked like one — `config set brightness` wrote the
    // field, `config save` put it in the file, and the panel came up at
    // whatever `Panel::Init` left it at regardless. A setting that survives a
    // reboot and changes nothing is worse than one that is missing, because
    // the operator has no reason to doubt it.
    //
    // Found by the reading form of `display brightness`, which is the whole
    // argument for that form existing: the live value and the stored one are
    // two numbers, and nothing else on this device prints them side by side.
    // Before the splash, so the first thing on the glass is already at the
    // brightness that was asked for rather than flashing full-scale first.
    if (board::Display().Ready()) {
        board::Display().SetBrightness(config::Get().display.brightness);
    }

    // **The splash and the boot sound are one event, and the order below is
    // what makes them one.** The picture goes on the glass first, the chime
    // plays under it, and LVGL takes the panel over only afterwards — a device
    // that lights up silently and then beeps at a clock reads as two devices.
    //
    // Both come after the console on purpose: between them they hold this task
    // for about three seconds, and a board that will not answer `status` until
    // a chime has finished is a board that looks hung.
    const int64_t splash_started = esp_timer_get_time();

    if (board::Display().Ready()) {
        // While LVGL still owns nothing — the moment the port registers a
        // display it starts flushing its own blank screen over whatever is
        // there, so this is not a step that can be moved later.
        const esp_err_t splash = display::BlitRaw(board::Display(), kSplashImage);
        if (splash != ESP_OK) {
            // A boot with no splash is a boot (§10.10's rule about staying up
            // to report), so this is a log line and not a branch.
            ESP_LOGW(TAG, "%s not shown: %s", kSplashImage, esp_err_to_name(splash));
        }
    }

    // **`PlayWav` blocks for the length of the file, and here that is the
    // point rather than a limitation**: the splash is already on the panel and
    // needs no CPU to stay there, so the three seconds this spends are three
    // seconds of splash. It is also why `kSplashMs` is a floor and not a
    // duration — the sound is what actually decides how long the picture is
    // up, and a shorter file simply gives the wait below something to do.
    if (board::Sound().Ready()) {
        const esp_err_t err = board::Sound().PlayWav(kBootSound);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "%s not played: %s", kBootSound, esp_err_to_name(err));
        }
    }

    // LVGL, and it is `main` that starts it rather than `board::Init` for the
    // same reason the volume is applied here: the panel is hardware, the task
    // that draws on it is an application decision (§10.14.2).
    if (board::Display().Ready()) {
        const int64_t shown_ms = (esp_timer_get_time() - splash_started) / 1000;
        if (shown_ms < kSplashMs) {
            vTaskDelay(pdMS_TO_TICKS(kSplashMs - shown_ms));
        }

        const esp_err_t err = display::LvglInit(board::Display(), &board::Touch());
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "LVGL not started: %s", esp_err_to_name(err));
        } else {
            // The screens (§10.8), and the hardware is handed to them rather
            // than reached for — the same shape `timesync` gets the RTC in. A
            // component that drew a battery by including `board.h` would be a
            // component that cannot be built without this board (§10.14.2).
            //
            // **This is where the two buttons get their meaning** (§10.8.4):
            // `BOOT` says allow, `PWR` says deny — and `PWR` doubles as the way
            // back to the clock when there is nothing to say no to. The indices
            // are `board.h`'s, and `main` is the one file that knows both which
            // button is which and what a verdict is.
            screens::Keys keys;
            keys.buttons = &board::Buttons();
            keys.allow = board::button::kBootIndex;
            keys.deny = board::button::kPwrIndex;

            const esp_err_t screens_err = screens::Init(&board::Pmic(), keys);
            if (screens_err != ESP_OK) {
                ESP_LOGE(TAG, "screens not started: %s", esp_err_to_name(screens_err));
            }
        }
    }
}
