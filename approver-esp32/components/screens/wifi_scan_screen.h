#pragma once

// The list of what is on the air, painted (CLAUDE.md §10.8.6). Every rule it
// draws was taken in `ui/wifi_view.h` — which names are worth listing, which one
// is selected, and which five of them are on the glass.
//
// **Read-and-pick, and nothing else.** There is no password on this screen and
// no way to type one: picking writes the name into the record the Wi-Fi screen
// was showing and goes back to it, which is the one thing the repository owner
// asked the list for.
//
// Two things about it that are decisions rather than layout:
//
//   * **a tap on a row picks it**, the way a tap on a settings row activates it
//     — and that is safe here for a reason worth stating rather than assuming:
//     nothing on this screen writes a file, so the worst a stray finger does is
//     put a name in a field that `config reload` or a reboot puts back;
//   * **an empty list and a refused scan are different screens.** The header
//     says which, because "there is nothing here" and "I could not look" send
//     somebody in different directions — §10.9's rule about `unknown` being an
//     honest state, on the one screen where the radio might be busy elsewhere.

#include <cstdint>

#include "esp_err.h"
#include "lvgl.h"
#include "wifi_view.h"

namespace screens {

inline constexpr int32_t kScanPad = 24;
inline constexpr int32_t kScanTitleTop = 18;
inline constexpr int32_t kScanTitleHeight = 70;

inline constexpr int32_t kScanRowTop = 76;
inline constexpr int32_t kScanRowStride = 76;
inline constexpr int32_t kScanRowHeight = 68;
inline constexpr int32_t kScanRowTextLeft = 22;
inline constexpr int32_t kScanRowTextTop = 17;

// The right-hand column: the signal and whether it is protected. Right-aligned
// rather than placed — `-100 wpa` and `-54 open` are different widths.
inline constexpr int32_t kScanNoteWidth = 150;

inline constexpr int32_t kScanHintTop = 452;

static_assert(kScanRowTop + (ui::kWifiScanRows - 1) * kScanRowStride + kScanRowHeight <=
                  kScanHintTop,
              "the scan rows would run into the hint");
static_assert(kScanHintTop + 20 <= 480, "the hint would run off the bottom of the glass");

class WifiScanScreen {
   public:
    WifiScanScreen() = default;
    WifiScanScreen(const WifiScanScreen &) = delete;
    WifiScanScreen &operator=(const WifiScanScreen &) = delete;

    esp_err_t Create(lv_obj_t *parent);
    bool Ready() const { return root_ != nullptr; }

    void SetVisible(bool visible);

    // Idempotent, and under the caller's LVGL lock. It reads the view directly
    // rather than being handed a copy of it, the way the limits screen does:
    // sixteen SSIDs is half a kilobyte, and there is nothing here to gather from
    // anywhere else.
    void Apply(const ui::WifiView &view);

    // The same handoff every touched screen here uses (§10.8.1).
    static constexpr uint8_t kNoRow = 0xFF;
    uint8_t TakeTap();
    bool TakeBack();

   private:
    static void RowClicked(lv_event_t *event);
    static void BackClicked(lv_event_t *event);

    struct Row {
        lv_obj_t *plate = nullptr;
        lv_obj_t *label = nullptr;
        lv_obj_t *note = nullptr;
    };

    lv_obj_t *root_ = nullptr;
    lv_obj_t *title_ = nullptr;
    lv_obj_t *count_ = nullptr;
    lv_obj_t *hint_ = nullptr;
    Row rows_[ui::kWifiScanRows] = {};

    char count_text_[20] = {};
    char row_text_[ui::kWifiScanRows][ui::kWifiSsidSize] = {};
    char note_text_[ui::kWifiScanRows][16] = {};

    uint8_t tapped_ = kNoRow;
    bool back_ = false;
    bool visible_ = false;
};

}  // namespace screens
