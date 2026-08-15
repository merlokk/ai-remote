#include "rawimage.h"

#include <cstdio>

#include "esp_log.h"
#include "storage.h"

namespace display {

namespace {

constexpr const char *TAG = "rawimage";

// The one strip buffer, static and DMA-capable because it is plain internal
// RAM on this chip. `esp_lcd_panel_draw_bitmap` hands it to the SPI DMA
// engine, so it may not live on a task stack.
uint8_t strip[kStripLines * kWidth * 2];

}  // namespace

esp_err_t BlitRaw(Panel &panel, const char *path) {
    if (!panel.Ready()) {
        return ESP_ERR_INVALID_STATE;
    }

    const int width = panel.Width();
    const int height = panel.Height();
    if (width > kWidth) {
        // The buffer is sized for this board's panel and nothing wider. Said
        // out loud rather than left to overflow.
        ESP_LOGE(TAG, "panel is %d wide, the strip buffer holds %d", width, kWidth);
        return ESP_ERR_INVALID_SIZE;
    }

    char full[storage::kMaxPathLength];
    if (!storage::ResolvePath(path, full, sizeof(full))) {
        return ESP_ERR_INVALID_ARG;
    }

    FILE *file = fopen(full, "rb");
    if (file == nullptr) {
        return ESP_ERR_NOT_FOUND;
    }

    const long expected = static_cast<long>(width) * height * 2;
    fseek(file, 0, SEEK_END);
    const long actual = ftell(file);
    fseek(file, 0, SEEK_SET);

    if (actual != expected) {
        // **With both numbers**, because the useful question is which one is
        // wrong: a file generated for another panel, or a half-written one.
        ESP_LOGE(TAG, "%s is %ld bytes, this panel needs %ld (%dx%d, rgb565)", full, actual,
                 expected, width, height);
        fclose(file);
        return ESP_ERR_INVALID_SIZE;
    }

    const size_t row_bytes = static_cast<size_t>(width) * 2;
    esp_err_t err = ESP_OK;

    for (int y = 0; y < height; y += kStripLines) {
        int lines = kStripLines;
        if (y + lines > height) {
            lines = height - y;
        }

        const size_t wanted = row_bytes * static_cast<size_t>(lines);
        if (fread(strip, 1, wanted, file) != wanted) {
            // The size was checked above, so a short read here means the
            // filesystem gave up mid-file. Stop rather than draw the previous
            // strip again.
            ESP_LOGE(TAG, "%s: short read at row %d", full, y);
            err = ESP_ERR_INVALID_SIZE;
            break;
        }

        // The bytes go out exactly as they are stored — the file is already in
        // the panel's byte order, which is the whole point of the format.
        err = esp_lcd_panel_draw_bitmap(panel.Handle(), 0, y, width, y + lines, strip);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "draw failed at row %d: %s", y, esp_err_to_name(err));
            break;
        }
    }

    fclose(file);
    return err;
}

}  // namespace display
