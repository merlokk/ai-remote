#pragma once

// The touch test and the calibration, painted (CLAUDE.md §10.8.5). Every rule it
// draws was taken in `ui/touch_cal.h`, which includes `<cstdint>` and nothing
// else and is where §10.11 runs the arithmetic and every refusal.
//
// **This is the one screen that must work while the thing it is testing does
// not**, and that shapes all of it:
//
//   * **nothing on it is touchable.** Not one widget, not even the way out —
//     because a correction that put every press in the wrong place would take
//     away exactly the buttons that fix it. `BOOT` starts a calibration, `KEY`
//     puts the correction back to none, `PWR` leaves; the glass is for showing
//     where the finger is, never for pressing;
//   * **the crosshair follows the *raw* point**, corrected by the calibration in
//     use, and both numbers are on screen. Two numbers is what makes this a test
//     rather than a picture: if they disagree with where the finger is, the
//     correction is what is wrong, and if they agree and the finger does not,
//     the axes are (§10.1's `swap_xy` / `mirror_x` / `mirror_y`, which are a
//     `board.h` question and not a runtime one);
//   * **it draws its own crosshair rather than moving a widget.** A widget moved
//     per frame is a widget LVGL invalidates twice per frame, and this screen is
//     redrawn at the rate a finger moves.

#include <cstdint>

#include "esp_err.h"
#include "lvgl.h"
#include "touch_cal.h"

namespace screens {

inline constexpr int32_t kTouchPad = 24;
inline constexpr int32_t kTouchTitleTop = 22;

// Where the readout sits while the test is running. Below the middle, so a
// finger in the top half is not covering the numbers it is producing.
inline constexpr int32_t kTouchReadoutTop = 300;
inline constexpr int32_t kTouchReadoutStride = 40;

// The crosshair and the cross a calibration asks for: an arm length and a stroke.
inline constexpr int32_t kTouchArm = 26;
inline constexpr int32_t kTouchStroke = 3;

// What the screen is showing, handed in by the task so that this file reads no
// state of its own — the same shape `StatusFacts` has next door.
struct TouchView {
    ui::TouchStage stage = ui::TouchStage::kTest;

    bool touching = false;
    uint16_t raw_x = 0;
    uint16_t raw_y = 0;
    uint16_t screen_x = 0;
    uint16_t screen_y = 0;

    // Where the next cross is, while collecting.
    uint8_t collected = 0;
    int16_t target_x = 0;
    int16_t target_y = 0;

    ui::TouchFit outcome = ui::TouchFit::kOk;
    ui::TouchCalibration calibration;
};

class TouchScreen {
   public:
    TouchScreen() = default;
    TouchScreen(const TouchScreen &) = delete;
    TouchScreen &operator=(const TouchScreen &) = delete;

    esp_err_t Create(lv_obj_t *parent);
    bool Ready() const { return root_ != nullptr; }

    void SetVisible(bool visible);

    // Idempotent, and under the caller's LVGL lock.
    void Apply(const TouchView &view);

   private:
    static void DrawMarks(lv_event_t *event);
    void Paint(lv_layer_t *layer, const lv_area_t &at) const;

    lv_obj_t *root_ = nullptr;
    lv_obj_t *canvas_ = nullptr;
    lv_obj_t *title_ = nullptr;
    lv_obj_t *hint_ = nullptr;
    lv_obj_t *line_[3] = {};

    char title_text_[40] = {};
    char hint_text_[48] = {};
    // Wide enough for the longest line either mode produces, which is the
    // correction itself: `x -1000/1000-480  y -1000/1000-480` and a terminator.
    char line_text_[3][56] = {};

    TouchView view_;
};

}  // namespace screens
