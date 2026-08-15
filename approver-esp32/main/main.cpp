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
#include "storage.h"

namespace {

constexpr const char *TAG = "app";

// Flashed with the image from `spiffs_image/` (§10.15). Uncompressed 16 kHz
// mono PCM, because the firmware has no decoder and does not want one — the
// argument is in `speaker.h`.
constexpr const char *kBootSound = "poweron.wav";

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
    board::Init();
    console::Init();

    // Settings applied to the hardware they belong to. `main` is where the two
    // meet: `config` knows nothing about a codec, and `board` knows nothing
    // about a file (§10.14.2).
    if (board::Codec().Present()) {
        board::Codec().SetVolume(config::Get().audio.volume_percent);
    }

    // The boot sound, last and deliberately after the console: it takes three
    // seconds of this task, and a device that will not answer `status` until a
    // chime has finished is a device that looks hung. Failure is a log line —
    // §10.10's rule holds here too, and a silent boot is a working boot.
    if (board::Sound().Ready()) {
        const esp_err_t err = board::Sound().PlayWav(kBootSound);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "%s not played: %s", kBootSound, esp_err_to_name(err));
        }
    }
}
