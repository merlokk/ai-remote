// The entry point. `main/` stays thin (CLAUDE.md §10.14.2): the library layer
// lives in `components/`, the logic on top of it, and neither exists yet.
//
// The one thing this does today is print which partition it booted from, which
// is what says the custom table of `partitions.csv` actually took effect.

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
#include "rawimage.h"
#include "storage.h"
#include "timezone.h"

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

// **A placeholder, and it is meant to be deleted.** §10.8 specifies five
// screens and none of them exists yet; what this draws is the smallest thing
// that answers the two questions a fresh display driver raises — are the
// colours and the byte order right, and does a press land where the finger did.
// A tap moves the dot. When the screens of §10.8 arrive, this goes with them,
// and `main` goes back to composing rather than drawing.
void ShowPlaceholder() {
    display::Lock lock;
    if (!lock) {
        return;
    }

    lv_obj_t *screen = lv_screen_active();
    // Black, because on an AMOLED an unlit pixel costs no power and no
    // lifetime — §10.8.1 makes that a design rule, and it may as well start
    // here.
    lv_obj_set_style_bg_color(screen, lv_color_black(), LV_PART_MAIN);

    lv_obj_t *title = lv_label_create(screen);
    lv_label_set_text(title, "approver-esp32");
    lv_obj_set_style_text_color(title, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -40);

    lv_obj_t *subtitle = lv_label_create(screen);
    lv_label_set_text(subtitle, "no screens yet - tap to test touch");
    lv_obj_set_style_text_color(subtitle, lv_palette_main(LV_PALETTE_GREY), LV_PART_MAIN);
    lv_obj_align(subtitle, LV_ALIGN_CENTER, 0, 0);

    // Three primaries in a row: if the swap_bytes flag were wrong these would
    // come out as three different, plausible, wrong colours rather than as
    // nothing — which is why the check is coloured squares and not a message.
    static const lv_color_t kSwatches[] = {
        lv_palette_main(LV_PALETTE_RED),
        lv_palette_main(LV_PALETTE_GREEN),
        lv_palette_main(LV_PALETTE_BLUE),
    };
    for (int i = 0; i < 3; ++i) {
        lv_obj_t *swatch = lv_obj_create(screen);
        lv_obj_set_size(swatch, 60, 60);
        lv_obj_set_style_bg_color(swatch, kSwatches[i], LV_PART_MAIN);
        lv_obj_set_style_border_width(swatch, 0, LV_PART_MAIN);
        lv_obj_remove_flag(swatch, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_align(swatch, LV_ALIGN_CENTER, (i - 1) * 70, 70);
    }

    lv_obj_t *dot = lv_obj_create(screen);
    lv_obj_set_size(dot, 24, 24);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(dot, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_border_width(dot, 0, LV_PART_MAIN);
    lv_obj_add_flag(dot, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(dot, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_add_flag(screen, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_user_data(screen, dot);
    lv_obj_add_event_cb(
        screen,
        [](lv_event_t *event) {
            auto *marker = static_cast<lv_obj_t *>(
                lv_obj_get_user_data(static_cast<lv_obj_t *>(lv_event_get_current_target(event))));
            lv_point_t point = {};
            lv_indev_get_point(lv_indev_active(), &point);
            lv_obj_remove_flag(marker, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(marker, point.x - 12, point.y - 12);
        },
        LV_EVENT_PRESSING, nullptr);
}

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
            ShowPlaceholder();
        }
    }
}
