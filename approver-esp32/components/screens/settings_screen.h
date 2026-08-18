#pragma once

// The settings list, painted (CLAUDE.md §10.8.5). Every rule it draws was taken
// in `ui/settings_menu.h`, which includes `<cstdint>` and nothing else and is
// where the tests are (§10.11) — which row is selected, which rows have nothing
// behind them yet, and how many presses it takes to restart the device.
//
// A **screen**, like the clock and the limits: three full-screen objects on the
// same LVGL screen, exactly one visible, and the request card of §10.8.4 as the
// overlay above all of them.
//
// **This is the first screen in this firmware that is touched**, and the two
// consequences are worth stating where somebody will read them:
//
//   * the rows are `LV_OBJ_FLAG_CLICKABLE` and a tap is recorded here rather
//     than acted on. `TakeTap` hands it to the screens task, which is the only
//     thing that may move the navigator — an event callback that navigated would
//     be doing it inside the LVGL task, which §10.8.1 forbids for the same reason
//     it forbids signing there;
//   * a tap that lands while a request card is up must reach nothing. That is
//     the caller's check rather than this file's, because the card is the
//     navigator's business — but the overlay is also made clickable so a finger
//     cannot fall through the gaps in it, which is §10.8.4's "nothing on the card
//     is touchable" from the other side.
//
// Nothing here is a decision. The order of the rows, the arming of the reboot
// and what an unbuilt row answers are all next door.

#include <cstdint>

#include "esp_err.h"
#include "lvgl.h"
#include "settings_menu.h"

namespace screens {

// --- The layout ----------------------------------------------------------
// A title that is also the way out, then one plate per row, full width, with
// generous gaps: this is a list touched with a finger on a 480×480 panel, and
// the thing a finger is worst at is small targets next to each other.
inline constexpr int32_t kSettingsPad = 24;
inline constexpr int32_t kSettingsTitleTop = 18;

// **These numbers shrank when the fifth row arrived, and the `static_assert`
// below is what noticed** — four rows at the old stride left 526 pixels of list
// on a 480-pixel panel, and the build refused it rather than the operator
// discovering that `power off` was drawn past the bottom of the glass. That is
// the whole reason the assertion exists: a layout constant is the one kind of
// mistake that looks fine in every test and only shows up on the panel.
inline constexpr int32_t kSettingsFirstRowTop = 76;
inline constexpr int32_t kSettingsRowStride = 80;
inline constexpr int32_t kSettingsRowHeight = 68;
inline constexpr int32_t kSettingsRowTextLeft = 22;
inline constexpr int32_t kSettingsRowTextTop = 17;

// The width of the right-hand note on a row — "soon" on a row with nothing
// behind it, "press again" on an armed reboot. Right-aligned rather than
// placed, for the reason §10.8.3 gives about its countdowns: the field's width
// changes with its value.
inline constexpr int32_t kSettingsNoteWidth = 230;

static_assert(kSettingsFirstRowTop +
                      (ui::SettingsMenu::kEntryCount - 1) * kSettingsRowStride +
                      kSettingsRowHeight <=
                  480,
              "the settings rows would run off the bottom of the glass");

// One row's widgets. Kept together because there are four of them and the only
// difference between them is the words.
struct MenuRow {
    lv_obj_t *plate = nullptr;
    lv_obj_t *label = nullptr;
    lv_obj_t *note = nullptr;
};

class SettingsScreen {
   public:
    SettingsScreen() = default;
    SettingsScreen(const SettingsScreen &) = delete;
    SettingsScreen &operator=(const SettingsScreen &) = delete;

    // Builds it under `parent`, hidden. **The caller holds the LVGL lock**, the
    // contract every screen here has.
    esp_err_t Create(lv_obj_t *parent);
    bool Ready() const { return root_ != nullptr; }

    void SetVisible(bool visible);

    // What to show. Idempotent, and under the caller's lock.
    void Apply(const ui::SettingsMenu &menu, uint32_t now_ms);

    // --- Touch ------------------------------------------------------------
    //
    // Both of these are written by an LVGL event callback and read by the
    // screens task **inside its own display lock**, which is the same lock the
    // callback runs under — so the handoff needs no mutex of its own. Taking one
    // reads and clears it: an intent acted on twice is a row activated twice,
    // and on the last row that is a device that restarts when it was asked to
    // ask.
    static constexpr uint8_t kNoRow = 0xFF;
    uint8_t TakeTap();
    bool TakeBack();

   private:
    static void RowClicked(lv_event_t *event);
    static void BackClicked(lv_event_t *event);

    lv_obj_t *root_ = nullptr;
    lv_obj_t *title_ = nullptr;
    MenuRow rows_[ui::SettingsMenu::kEntryCount] = {};

    uint8_t tapped_ = kNoRow;
    bool back_ = false;

    uint8_t shown_selected_ = kNoRow;
    bool shown_armed_ = false;
    bool shown_can_power_off_ = false;
    bool visible_ = false;
};

}  // namespace screens
