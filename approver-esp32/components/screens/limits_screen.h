#pragma once

// The limits screen, painted (CLAUDE.md §10.8.3). Every decision it shows was
// taken in `ui/limits_view.h`, which is host-tested; this file has no rules in it,
// only a layout and a palette.
//
// It is a **screen**, not an overlay: the clock and this one are two full-screen
// objects on the same LVGL screen and exactly one is visible, which is what the
// navigator decides. The request card of §10.8.4 is the overlay, and it sits over
// whichever of the two is up — so a permission request arriving while the limits
// are on the glass covers them and leaves them exactly as they were underneath.
//
// **A readout and nothing more.** Nothing here is touchable and nothing here can
// be acted on; §10.8.3's test is that deleting this screen leaves a working
// responder, and the way to keep that true is for it to have no way to reach one.

#include <cstdint>

#include "esp_err.h"
#include "limits_view.h"
#include "lvgl.h"

namespace screens {

// --- The layout ----------------------------------------------------------
// Three gauges down the middle of a 480×480 panel, the model above them and the
// session under them. Wide bars rather than tall ones: the number that matters is
// how far along a window is, and length is what the eye reads that from.
//
// **One size for everything that matters, and that is a measurement rather than
// a preference.** The first version had the labels and the countdowns at 14
// point, which is a size for something you lean in to read — and this is an
// object on a desk looked at from across a room. They are 28 now.
//
// The percentage went to 48 with them, for half an hour, and then came back.
// `sdkconfig.defaults` *enables* Montserrat 48 for the clock, but the clock draws
// its digits as seven segments (§10.8.2) and never references the font — so the
// linker had been dropping it, and the first thing to name it cost **97,280 bytes
// of flash**, measured. §10.8.2 already refused a generated font on exactly that
// ground, and paying it here for one size step would be that decision reversed
// for a smaller reason.
//
// So the hierarchy is carried by **colour and by the bar** instead of by size: the
// percentage is the bright thing in its row, the label and the countdown are
// faint, and the coloured length underneath is what the eye actually reads the
// magnitude from. Nothing new is compiled in, and the app is where it was.
inline constexpr int32_t kLimitsPad = 28;

inline constexpr int32_t kLimitsModelTop = 30;
inline constexpr int32_t kLimitsEffortTop = 74;

inline constexpr int32_t kLimitsFirstGaugeTop = 116;
inline constexpr int32_t kLimitsGaugeStride = 104;

// Within a row: the label, the percentage and the countdown share one line, and
// the bar goes underneath all three.
inline constexpr int32_t kLimitsTextTop = 16;
inline constexpr int32_t kLimitsPercentLeft = 104;
inline constexpr int32_t kLimitsBarTop = 58;
inline constexpr int32_t kLimitsBarHeight = 26;

// **The countdown is right-aligned rather than placed**, because it is the one
// field whose width changes with its value: `now` is three characters and
// `23h59m` is six, and at 28 point that difference is sixty pixels. A left-placed
// label would have to be positioned for the longest one and would then float away
// from the edge for every shorter one.
inline constexpr int32_t kLimitsCountdownWidth = 220;

inline constexpr int32_t kLimitsCwdTop = 426;
inline constexpr int32_t kLimitsAgeTop = 450;

// One gauge's widgets, kept together because there are three of them and the only
// difference between them is the label and which scale colours the bar.
struct GaugeRow {
    lv_obj_t *label = nullptr;
    lv_obj_t *percent = nullptr;
    lv_obj_t *track = nullptr;
    lv_obj_t *fill = nullptr;
    lv_obj_t *countdown = nullptr;

    char percent_text[8] = {};
    char countdown_text[ui::kResetsTextSize] = {};

    // What is on the glass, so a screen refreshed ten times a second repaints
    // nothing when nothing changed — the same reason the request card keeps its
    // countdown in whole seconds.
    uint8_t shown_percent = 0xFF;
    bool shown_present = false;
};

class LimitsScreen {
   public:
    LimitsScreen() = default;
    LimitsScreen(const LimitsScreen &) = delete;
    LimitsScreen &operator=(const LimitsScreen &) = delete;

    // Builds it under `parent`, hidden. **The caller holds the LVGL lock**, the
    // same contract the other two screens have.
    esp_err_t Create(lv_obj_t *parent);
    bool Ready() const { return root_ != nullptr; }

    // Show or hide the whole screen. Separate from `Apply` because the navigator
    // decides which screen is up and the view decides what is on it — two
    // questions, and conflating them is how a screen ends up visible with stale
    // numbers on it.
    void SetVisible(bool visible);

    // What to show. Idempotent, and under the caller's lock. `epoch_now` is the
    // device's idea of the time or 0 — it decides whether a countdown is computed
    // or taken from what the publisher resolved (§10.8.3).
    void Apply(const ui::LimitsView &view, int64_t epoch_now, uint32_t now_ms);

   private:
    void ApplyGauge(GaugeRow *row, const ui::Gauge &gauge, ui::Level level, const char *countdown);

    lv_obj_t *root_ = nullptr;
    lv_obj_t *model_ = nullptr;
    lv_obj_t *effort_ = nullptr;
    lv_obj_t *cwd_ = nullptr;
    lv_obj_t *age_ = nullptr;
    lv_obj_t *empty_ = nullptr;

    GaugeRow five_hour_;
    GaugeRow seven_day_;
    GaugeRow context_;

    char model_text_[ui::kModelNameSize] = {};
    char effort_text_[ui::kEffortSize + 8] = {};
    char cwd_text_[ui::kSessionCwdSize] = {};
    char age_text_[32] = {};

    uint32_t shown_seconds_ = 0xFFFFFFFFu;
    uint32_t shown_received_ = 0xFFFFFFFFu;
    bool visible_ = false;
};

}  // namespace screens
