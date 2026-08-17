#include "lvgl_display.h"

#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

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

// The screenshot of `lvgl_display.h`, and all of its state. One capture at a
// time, so there is one of these rather than a handle — and it is static, like
// everything else in this firmware (§10.14.1).
struct CaptureState {
    CaptureSink sink = nullptr;
    void *user = nullptr;

    // Written by the caller's task and read by the LVGL task, so `volatile` —
    // and that is all the synchronisation these need: the caller arms under the
    // LVGL lock and then only ever reads.
    volatile bool armed = false;
    volatile bool done = false;

    uint32_t pieces = 0;

    SemaphoreHandle_t finished = nullptr;
    StaticSemaphore_t finished_storage = {};
};

CaptureState capture;

// How long `Capture` waits for the LVGL lock in order to arm. Nothing to do with
// how long the frame itself may take — see the note at the call site.
constexpr uint32_t kCaptureArmMs = 500;

// Fires once per rendered piece, **before** the port writes it to the panel —
// which is why the pixels here are still LVGL's little-endian RGB565 rather than
// the big-endian this glass wants (`lvgl_display.h` says so, because a decoder
// on the other end has to know).
//
// The event carries the area; the buffer comes from `lv_display_get_buf_active`,
// which is the one the piece was rendered into: LVGL swaps its two buffers
// *after* the flush callback returns, so during this event the active one and the
// one being flushed are the same. `lv_refr.c` is where that ordering lives, and
// it is the only reason this hook works at all.
void FlushStartCb(lv_event_t *event) {
    if (!capture.armed) {
        return;
    }
    auto *area = static_cast<lv_area_t *>(lv_event_get_param(event));
    lv_draw_buf_t *buf = lv_display_get_buf_active(lv_display);
    if (area == nullptr || buf == nullptr || buf->data == nullptr) {
        return;
    }

    capture.pieces++;
    if (capture.sink != nullptr) {
        capture.sink(*area, buf->header.stride, buf->data, capture.user);
    }

    // LVGL sets this before calling the flush callback, so it is already true on
    // the last piece — which makes "the frame is complete" a fact to read rather
    // than a row count to add up and get wrong.
    if (lv_display_flush_is_last(lv_display)) {
        capture.armed = false;
        capture.done = true;
        xSemaphoreGive(capture.finished);
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

        // Registered once and disarmed, rather than added and removed around
        // each capture: what it costs while nobody is looking is one load and
        // one branch per flush, and what adding it at runtime would cost is a
        // list being edited from a task other than the one walking it.
        capture.finished = xSemaphoreCreateBinaryStatic(&capture.finished_storage);
        if (capture.finished == nullptr) {
            ESP_LOGE(TAG, "capture semaphore not created");
        } else {
            lv_display_add_event_cb(lv_display, FlushStartCb, LV_EVENT_FLUSH_START, nullptr);
        }

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

esp_err_t Capture(CaptureSink sink, void *user, uint32_t timeout_ms) {
    if (!started || lv_display == nullptr || capture.finished == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    if (sink == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (capture.armed) {
        return ESP_ERR_INVALID_STATE;
    }

    {
        // **A short wait for the lock and a long one for the frame**, which is
        // not the same number: arming takes microseconds, and the sink is what
        // takes seconds. Passing `timeout_ms` to both would mean a command that
        // sits on the LVGL lock for half a minute if something else is holding
        // it.
        Lock lock(kCaptureArmMs);
        if (!lock) {
            return ESP_ERR_TIMEOUT;
        }
        capture.sink = sink;
        capture.user = user;
        capture.pieces = 0;
        capture.done = false;
        // Drain a give left by a capture that timed out: the piece that arrived
        // late still signalled, and inheriting that would make the next capture
        // return before it had anything.
        xSemaphoreTake(capture.finished, 0);
        capture.armed = true;

        // The whole screen, because a partial screenshot of a screen that only
        // repaints its digits would be a picture of the digits.
        lv_obj_invalidate(lv_screen_active());
    }

    // **Not under the lock**, and that is the one thing in here that has to be
    // right: the work happens in the LVGL task, and waiting for it while holding
    // its lock is a deadlock rather than a slow command.
    if (xSemaphoreTake(capture.finished, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        capture.armed = false;
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

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
