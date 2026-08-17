#pragma once

// LVGL on top of the panel — still the library layer (CLAUDE.md §10.14.2).
//
// What this produces is an `lv_display_t` and an `lv_indev_t`, and the lock
// that guards them. It does not know what a screen is: the five of §10.8 are
// logic and live above this, and the rule they inherit from here is the one
// §10.8.1 states — **one task owns the display.** LVGL is not thread-safe, so
// every widget touch happens either in the port's own task or under `Lock`.
//
// The port is `espressif/esp_lvgl_port`, not the `esp_lvgl_adapter` the vendor
// example uses; `main/idf_component.yml` argues that where it is declared.
//
//     if (display::Lock lock; lock) {
//         lv_label_set_text(label, "…");    // safe here, and released on exit
//     }
//
// A signature, a socket read and a JSON parse never run inside an LVGL callback
// and never hold this lock — §10.1 (one core) is why, and §10.8.1 turns it into
// a code rule.

#include <cstdint>

#include "esp_err.h"
#include "lvgl.h"
#include "panel.h"
#include "touch.h"

namespace display {

// The LVGL task's stack. Larger than it looks like it needs to be: LVGL's
// renderer recurses through the widget tree, and a stack overflow inside a
// flush is a panic naming the timer rather than the layout that caused it.
inline constexpr uint32_t kLvglTaskStack = 8 * 1024;

// Above the bus task and below anything with a deadline. The frame the operator
// is looking at is what this protects (§10.8.1).
inline constexpr int kLvglTaskPriority = 4;

// Brings up the port, registers the panel as a display, and — if `touch` is
// ready — registers it as a pointer input. A null or unready `touch` is not an
// error: the device still shows a clock, it just cannot be pressed, and saying
// so on the screen is §10.8.2's job rather than this file's.
esp_err_t LvglInit(Panel &panel, Touch *touch);

bool LvglReady();

lv_display_t *LvglDisplay();

// --- A screenshot, streamed rather than stored ----------------------------
//
// **The panel cannot be read back and there is nowhere to keep a frame.** QSPI
// to the CO5300 is write-only, and a 480×480 frame at 16 bpp is 460,800 bytes
// against 512 KB of SRAM shared with lwIP, Wi-Fi and LVGL's own pool, with no
// PSRAM and no way to add it (§10.1). `lv_snapshot_take` wants exactly that
// frame and is therefore not an option on this part.
//
// So the capture happens where the pixels already pass through in pieces: LVGL
// renders a partial buffer at a time and this hooks `LV_EVENT_FLUSH_START`,
// which fires with the area about to be written and with the buffer that was
// rendered into. Each piece is handed to the caller and then forgotten — the
// only memory this costs is whatever the caller's sink needs.
//
// Three things a caller has to know:
//
//   * **the sink runs in the LVGL task**, once per piece, before the panel is
//     written. §10.8.1 forbids blocking work there and this deliberately does
//     it: a sink that writes to the console holds up the frame for as long as
//     that takes. Acceptable for a command somebody typed, and not a pattern to
//     copy;
//   * **the pixels are LVGL-native little-endian RGB565.** The byte swap this
//     panel needs happens inside the port's own flush callback, which is *after*
//     this event, so what the sink sees is not what goes on the wire — and it is
//     the order a host-side decoder wants anyway;
//   * **`stride` is bytes per row and may exceed the area's width**, so a row is
//     `(x2 - x1 + 1) * 2` bytes taken from `rows + y * stride`.
using CaptureSink = void (*)(const lv_area_t &area, uint32_t stride, const uint8_t *rows,
                             void *user);

// Invalidates the whole screen and streams the refresh that follows. Blocks
// until the last piece has been handed over, or `timeout_ms` passes. One at a
// time — a second caller is refused rather than interleaved.
esp_err_t Capture(CaptureSink sink, void *user, uint32_t timeout_ms);

// The scope guard, the same shape `i2cbus::Lease` has and for the same reason
// (§10.14.1): releasing is the destructor, so it is not a line anyone can
// forget on an early return.
class Lock {
   public:
    // 0 waits indefinitely. Prefer a bound in anything that runs periodically —
    // an LVGL lock waited on forever from a bus task is a hang that looks like
    // a network problem.
    explicit Lock(uint32_t timeout_ms = 0);
    ~Lock();

    Lock(const Lock &) = delete;
    Lock &operator=(const Lock &) = delete;

    bool Held() const { return held_; }
    explicit operator bool() const { return held_; }

   private:
    bool held_ = false;
};

}  // namespace display
