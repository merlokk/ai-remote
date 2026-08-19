#pragma once

// The Wi-Fi screen, painted (CLAUDE.md §10.8.6). Every rule it draws was taken
// in `ui/wifi_view.h`, which includes `<cstdint>` and `<cstddef>` and nothing
// else and is where §10.11 runs the mode cycle, the stepping and every refusal.
//
// What is on it, top to bottom: the mode — off, client or access point — then
// the record that mode names, with arrows when there is more than one of them,
// then that record's name and password, then the row that opens the list of
// what is on the air.
//
// Three things about it that are decisions rather than layout:
//
//   * **the password is drawn in full.** There is no keyboard yet (§10.8.6 has
//     that screen in millimetres), so this row is read and never typed — and a
//     value that is masked and cannot be revealed is a row that answers no
//     question at all. What that costs is stated where it belongs: §10.15 keeps
//     a password out of every log line and every console dump, and this screen
//     is the one place it is meant to be visible, to somebody holding the
//     device;
//   * **a long value drops to the small font rather than being cut.** An SSID
//     is 32 bytes and a WPA key is 63, and neither fits the value column at
//     Montserrat 28. §10.8.4's rule about never truncating something somebody
//     is about to act on applies here too — so the two sizes that
//     `sdkconfig.defaults` already enables are used as a fallback, which costs
//     no flash (§10.8.3 measured what a *third* size costs: 97 KB);
//   * **the arrows are their own touch targets inside the record row**, and the
//     row itself steps forward when it is pressed. That is what makes one
//     button enough — `BOOT` walks the rows, `KEY` presses one — on a screen
//     whose whole job is to be usable when the glass is suspect.

#include <cstdint>

#include "esp_err.h"
#include "lvgl.h"
#include "wifi_view.h"

namespace screens {

// --- The layout ----------------------------------------------------------
inline constexpr int32_t kWifiPad = 24;
inline constexpr int32_t kWifiTitleTop = 18;
inline constexpr int32_t kWifiTitleHeight = 70;

// The two rows above the fields, and the one below them.
inline constexpr int32_t kWifiRowTop = 74;
inline constexpr int32_t kWifiRowStride = 72;
inline constexpr int32_t kWifiRowHeight = 64;
inline constexpr int32_t kWifiRowTextLeft = 22;
inline constexpr int32_t kWifiRowTextTop = 15;
// The right-hand note on a row: the mode's own word, or `no record` on the scan
// row when there is nothing to fill. **150 rather than 200, because the scan
// row's label grew.** `scan networks` is thirteen characters and about 200 px at
// Montserrat 28, so a 200-px note column starting at 210 would have had the two
// sharing pixels the moment the note said anything — which is the collision
// §10.8.5 found on the status page's label column, seen coming this time.
inline constexpr int32_t kWifiNoteWidth = 150;

// The name and the password: a faint label on the left and the value in a
// column wide enough to wrap onto a second line.
inline constexpr int32_t kWifiFieldTop = 220;
inline constexpr int32_t kWifiFieldStride = 74;
inline constexpr int32_t kWifiValueLeft = 176;
inline constexpr int32_t kWifiValueWidth = 480 - kWifiPad - kWifiValueLeft;

inline constexpr int32_t kWifiScanRowTop = 372;
inline constexpr int32_t kWifiHintTop = 446;

// Where the small font takes over, counted at Montserrat 28 across
// `kWifiValueWidth` on two lines rather than guessed — §10.8.5 records what a
// character budget counted with the wrong characters costs.
inline constexpr size_t kWifiValueBigMax = 24;

static_assert(kWifiScanRowTop + kWifiRowHeight <= kWifiHintTop,
              "the scan row would run into the hint");
static_assert(kWifiHintTop + 20 <= 480, "the hint would run off the bottom of the glass");

// What the screen shows that is **not** in `ui::WifiView`: the record's own two
// strings, and one line about what the radio is actually doing. Gathered by the
// screens task, the same shape `StatusFacts` has next door — this file reads no
// state of its own and has never heard of `config.json`.
struct WifiFacts {
    char ssid[ui::kWifiSsidSize] = {};
    char password[ui::kWifiPasswordSize] = {};

    // `wifimgr`'s current state, in its own words: `online`, `connecting`,
    // `temporary ap`. **Desired and current side by side** is §10.9's whole
    // shape, and the mode row above is the desired half.
    char state[28] = {};
};

class WifiScreen {
   public:
    WifiScreen() = default;
    WifiScreen(const WifiScreen &) = delete;
    WifiScreen &operator=(const WifiScreen &) = delete;

    esp_err_t Create(lv_obj_t *parent);
    bool Ready() const { return root_ != nullptr; }

    void SetVisible(bool visible);

    // Idempotent, and under the caller's LVGL lock.
    void Apply(const ui::WifiView &view, const WifiFacts &facts, uint32_t now_ms);

    // --- Touch ------------------------------------------------------------
    //
    // The same handoff the settings list uses and for the same reason: an LVGL
    // callback records what a finger did and the screens task is the only thing
    // that may act on it (§10.8.1).
    static constexpr uint8_t kNoRow = 0xFF;
    uint8_t TakeTap();

    // -1, 0 or +1: which arrow was pressed, if either. Its own value rather than
    // a row, because an arrow is not a row — pressing it must not move the
    // selection to something a later press would then act on.
    int8_t TakeArrow();

    bool TakeBack();

   private:
    static void RowClicked(lv_event_t *event);
    static void ArrowClicked(lv_event_t *event);
    static void BackClicked(lv_event_t *event);

    struct Row {
        lv_obj_t *plate = nullptr;
        lv_obj_t *label = nullptr;
        lv_obj_t *note = nullptr;
    };

    void SetValue(lv_obj_t *label, char *cache, size_t size, const char *text);

    lv_obj_t *root_ = nullptr;
    lv_obj_t *title_ = nullptr;
    lv_obj_t *state_ = nullptr;
    Row rows_[ui::WifiView::kRowCount] = {};
    lv_obj_t *left_ = nullptr;
    lv_obj_t *right_ = nullptr;
    lv_obj_t *ssid_ = nullptr;
    lv_obj_t *password_ = nullptr;
    lv_obj_t *hint_ = nullptr;

    char state_text_[28] = {};
    char record_text_[24] = {};
    char mode_text_[12] = {};
    char ssid_text_[ui::kWifiSsidSize] = {};
    char password_text_[ui::kWifiPasswordSize] = {};
    char hint_text_[64] = {};

    uint8_t tapped_ = kNoRow;
    int8_t arrow_ = 0;
    bool back_ = false;
    bool visible_ = false;
};

}  // namespace screens
