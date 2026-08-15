// The entry point. `main/` stays thin (CLAUDE.md §10.14.2): the library layer
// lives in `components/`, the logic on top of it, and neither exists yet.
//
// The one thing this does today is print which partition it booted from, which
// is what says the custom table of `partitions.csv` actually took effect.

#include <cinttypes>

#include "board.h"
#include "console.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "storage.h"

namespace {

constexpr const char *TAG = "app";

}  // namespace

extern "C" void app_main(void) {
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_app_desc_t *desc = esp_app_get_description();

    ESP_LOGI(TAG, "%s %s, running from %s at 0x%" PRIx32 " (%" PRIu32 " KB)",
             desc->project_name, desc->version, running->label,
             running->address, running->size / 1024);

    board::LogPinout();

    // Order matters and is written down rather than implied (§10.14.1): the
    // I²C bus and the chips on it, then the filesystem, then the console — so
    // that `power` and `cat` both have something to answer with the moment the
    // prompt appears. None of these failures is fatal: a device that cannot
    // mount its storage or reach its PMIC should still come up far enough to
    // say so.
    board::Init();
    storage::Init();
    console::Init();
}
