#include "lvgl_display.h"

#include "esp_log.h"
#include "esp_lvgl_port.h"

namespace display {

namespace {

constexpr const char *TAG = "lvgl";

lv_display_t *lv_display = nullptr;
lv_indev_t *lv_pointer = nullptr;
bool started = false;

// **This panel only accepts even coordinates, and nothing says so out loud.**
// A flush whose window starts or ends on an odd pixel is written to the wrong
// place — the symptom is a screen that is almost right, torn by a column, and
// only where something small was redrawn. So every invalidated area is grown to
// even/odd bounds before LVGL renders it. Straight out of the vendor example,
// which is the only reason this is known at all.
void RoundAreaCb(lv_event_t *event) {
    auto *area = static_cast<lv_area_t *>(lv_event_get_param(event));
    if (area == nullptr) {
        return;
    }
    area->x1 = (area->x1 >> 1) << 1;
    area->y1 = (area->y1 >> 1) << 1;
    area->x2 = ((area->x2 >> 1) << 1) + 1;
    area->y2 = ((area->y2 >> 1) << 1) + 1;
}

// The pointer LVGL polls. It calls `Touch::Read`, which takes the I²C lease —
// see the argument in `touch.h` for why the port's own `lvgl_port_add_touch` is
// not used here.
void TouchReadCb(lv_indev_t *indev, lv_indev_data_t *data) {
    auto *touch = static_cast<Touch *>(lv_indev_get_user_data(indev));
    if (touch == nullptr) {
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }

    uint16_t x = 0;
    uint16_t y = 0;
    if (touch->Read(&x, &y)) {
        data->point.x = x;
        data->point.y = y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        // A released state on a missed read is correct rather than convenient:
        // LVGL treats a gap as a release, and inventing a held press out of a
        // bus timeout would be a press nobody made. §10.10's rule about never
        // inventing input, one layer down from where it is usually quoted.
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

}  // namespace

esp_err_t LvglInit(Panel &panel, Touch *touch) {
    if (started) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!panel.Ready()) {
        return ESP_ERR_INVALID_STATE;
    }

    lvgl_port_cfg_t port = ESP_LVGL_PORT_INIT_CONFIG();
    port.task_priority = kLvglTaskPriority;
    port.task_stack = kLvglTaskStack;
    // One core, so there is nothing to pin to (§10.1). Said as a value rather
    // than left at the default, because the vendor's example pins to core 0 and
    // reading that on a single-core part is a minute wasted.
    port.task_affinity = -1;
    port.timer_period_ms = 5;
    port.task_max_sleep_ms = 500;

    esp_err_t err = lvgl_port_init(&port);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "port not started: %s", esp_err_to_name(err));
        return err;
    }

    lvgl_port_display_cfg_t cfg = {};
    cfg.io_handle = panel.Io();
    cfg.panel_handle = panel.Handle();
    cfg.buffer_size = panel.FlushBufferPixels();
    cfg.double_buffer = true;
    cfg.hres = panel.Width();
    cfg.vres = panel.Height();
    cfg.monochrome = false;
    cfg.color_format = LV_COLOR_FORMAT_RGB565;
    cfg.flags.buff_dma = true;
    // There is no PSRAM on this chip and no way to add it (§10.1). Explicit so
    // that a copied config from a board that has it fails to compile a lie.
    cfg.flags.buff_spiram = false;
    // **The panel wants RGB565 big-endian and LVGL renders little.** Done here,
    // in the flush path, rather than through `CONFIG_LV_COLOR_16_SWAP` — a
    // global that is invisible from the driver that needs it.
    cfg.flags.swap_bytes = true;
    // Partial redraw. `full_refresh` with a 460 KB frame is the thing this
    // chip cannot do.
    cfg.flags.full_refresh = false;
    cfg.flags.direct_mode = false;

    // **This logs one error line that means nothing, and it is not ours to
    // suppress.** `lvgl_port_add_disp` calls `esp_lcd_panel_swap_xy` whatever
    // the rotation is, and the SH8601 driver answers
    //
    //   E sh8601: swap_xy is not supported by this panel
    //
    // for the `false` we asked for. Harmless — this panel is used at rotation
    // 0, and its orientation is the 36h command in `panel.cpp`'s init list
    // rather than anything LVGL does — but it is an `E` at every boot, and an
    // `E` nobody has explained is an `E` somebody will chase.
    lv_display = lvgl_port_add_disp(&cfg);
    if (lv_display == nullptr) {
        ESP_LOGE(TAG, "display not registered");
        return ESP_FAIL;
    }

    // The port's task is already running, so everything below takes the lock —
    // including creating widgets, which is exactly what §10.8.1 will require of
    // the screens.
    {
        Lock lock;
        if (!lock) {
            ESP_LOGE(TAG, "could not take the LVGL lock at startup");
            return ESP_ERR_TIMEOUT;
        }

        lv_display_add_event_cb(lv_display, RoundAreaCb, LV_EVENT_INVALIDATE_AREA, nullptr);

        if (touch != nullptr && touch->Ready()) {
            lv_pointer = lv_indev_create();
            if (lv_pointer == nullptr) {
                ESP_LOGE(TAG, "pointer not created");
            } else {
                lv_indev_set_type(lv_pointer, LV_INDEV_TYPE_POINTER);
                lv_indev_set_read_cb(lv_pointer, TouchReadCb);
                lv_indev_set_display(lv_pointer, lv_display);
                lv_indev_set_user_data(lv_pointer, touch);
            }
        }
    }

    started = true;
    ESP_LOGI(TAG, "LVGL %d.%d.%d on %dx%d, %u px buffers x2, touch %s", LVGL_VERSION_MAJOR,
             LVGL_VERSION_MINOR, LVGL_VERSION_PATCH, panel.Width(), panel.Height(),
             static_cast<unsigned>(panel.FlushBufferPixels()),
             lv_pointer != nullptr ? "attached" : "absent");
    return ESP_OK;
}

bool LvglReady() { return started; }

lv_display_t *LvglDisplay() { return lv_display; }

Lock::Lock(uint32_t timeout_ms) {
    if (!started && lv_display == nullptr) {
        // Before `lvgl_port_init` there is nothing to lock and nothing running
        // that could race — but a caller that thinks it is locked when it is
        // not is worse than one that knows it failed.
        held_ = false;
        return;
    }
    held_ = lvgl_port_lock(static_cast<int>(timeout_ms));
}

Lock::~Lock() {
    if (held_) {
        lvgl_port_unlock();
    }
}

}  // namespace display
