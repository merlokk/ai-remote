#pragma once

// The clock — the home screen (CLAUDE.md §10.8.2), and the first of §10.8's
// five to exist.
//
// This file is the *painting* half. Every decision it draws was taken next door
// in `ui/clock_face.h`, which includes `<cstdint>` and nothing else and is
// therefore where the tests are (§10.11): whether the time is believable, how
// many bars, whether the bus dot is green, where on the glass the face sits this
// second and how far along the water is. Nothing here decides anything — it
// turns a `ui::ClockView` into pixels, and the only judgement in it is which
// green.
//
// Three things about how it is built, all of them §10.14.1 or §10.8.1:
//
//   * **it is built once and kept.** LVGL objects come out of a 64 KB pool
//     sized in `sdkconfig.defaults`; §10.8.1 forbids create-and-delete per
//     navigation for a different reason (the screen underneath has to come back
//     with its state), and this makes it expensive as well;
//   * **the digits are drawn, not typed.** LVGL's built-in fonts stop at
//     Montserrat 48 and a clock on a 480×480 panel wants glyphs three times
//     that; the alternatives were a generated font (tens of kilobytes of flash
//     and a new host-side tool under root §1) or shapes. Seven segments are
//     shapes, and they come with §10.8.2's `--:--` for free: a dash is not a
//     missing glyph, it is the middle segment on its own;
//   * **the whole face is one object that moves.** The drift that protects the
//     panel is `lv_obj_set_pos` on the parent, so nothing below has to know it
//     is happening.
//
// The geometry is in the header because the LVGL host preview (§10.12.1) is
// where a layout is argued about, and a snippet that has to guess the numbers is
// a snippet that previews something else.

#include <cstdint>

#include "clock_face.h"
#include "esp_err.h"
#include "lvgl.h"

namespace screens {

// --- The layout ----------------------------------------------------------
// One column of indicators on the left, four digits and a colon to the right of
// it, the date underneath. Sized so that the face plus the drift of
// `ui::ClockFace` fits the panel with a margin — asserted below rather than
// eyeballed.

inline constexpr int32_t kPanelWidth = 480;
inline constexpr int32_t kPanelHeight = 480;

// The indicator column: Wi-Fi, bus, battery, and the battery's percentage under
// it. Its width is what "100%" needs at Montserrat 28, which is the widest thing
// in it.
inline constexpr int32_t kColumnWidth = 76;
inline constexpr int32_t kColumnGap = 16;

inline constexpr int32_t kWifiWidth = 46;
inline constexpr int32_t kWifiHeight = 36;
inline constexpr int32_t kWifiTop = 0;

inline constexpr int32_t kBusDiameter = 30;
inline constexpr int32_t kBusTop = 50;

inline constexpr int32_t kBatteryWidth = 58;
inline constexpr int32_t kBatteryHeight = 28;
inline constexpr int32_t kBatteryTop = 94;
inline constexpr int32_t kBatteryNub = 5;  // the terminal bump on the right

inline constexpr int32_t kPercentTop = 124;

// One digit, and the segments inside it. `kSegmentGap` is the dark line between
// two segments that meet at a corner — without it a `0` is a rounded rectangle.
inline constexpr int32_t kDigitWidth = 64;
inline constexpr int32_t kDigitHeight = 146;
inline constexpr int32_t kStroke = 10;
inline constexpr int32_t kSegmentGap = 3;
inline constexpr int32_t kDigitGap = 12;
inline constexpr int32_t kColonWidth = 14;
inline constexpr int32_t kColonDot = 12;

// Four digits, the colon, and the four gaps between the five of them.
inline constexpr int32_t kDigitsWidth = 4 * kDigitWidth + 4 * kDigitGap + kColonWidth;

inline constexpr int32_t kFaceWidth = kColumnWidth + kColumnGap + kDigitsWidth;

// The tallest of the two columns, then the date under it.
inline constexpr int32_t kDateTop = 172;
inline constexpr int32_t kDateHeight = 34;
inline constexpr int32_t kFaceHeight = kDateTop + kDateHeight;

// Where the face rests when the drift is at zero: centred on the *wander*, so
// the middle of the walk is the middle of the panel rather than one corner of it.
inline constexpr int32_t kRestX = (kPanelWidth - kFaceWidth) / 2;
inline constexpr int32_t kRestY = (kPanelHeight - kFaceHeight) / 2;

static_assert(kRestX - ui::ClockFace::kDriftX >= 0 &&
                  kRestX + kFaceWidth + ui::ClockFace::kDriftX <= kPanelWidth,
              "the drift would push the face off the left or right of the glass");
static_assert(kRestY - ui::ClockFace::kDriftY >= 0 &&
                  kRestY + kFaceHeight + ui::ClockFace::kDriftY <= kPanelHeight,
              "the drift would push the face off the top or bottom of the glass");

// --- The screen ----------------------------------------------------------

class ClockScreen {
   public:
    ClockScreen() = default;
    ClockScreen(const ClockScreen &) = delete;
    ClockScreen &operator=(const ClockScreen &) = delete;

    // Builds the widgets under `parent`. **The caller holds the LVGL lock** —
    // §10.8.1's one-task rule, and this class never takes it itself so that a
    // caller cannot end up nesting it.
    esp_err_t Create(lv_obj_t *parent);
    bool Ready() const { return face_ != nullptr; }

    // What to show, and it is idempotent: called twenty times a second with the
    // same view it invalidates nothing. Also under the caller's lock.
    void Apply(const ui::ClockView &view);

    // The date line, separately because it is the one string this screen shows
    // that the model has no opinion about — a zone turns into words at the edge
    // (§10.8.2), and that edge is the caller. Empty hides it.
    void SetDate(const char *text);

   private:
    static void DrawDigits(lv_event_t *event);
    static void DrawIcons(lv_event_t *event);

    void PaintDigits(lv_layer_t *layer, const lv_area_t &at) const;
    void PaintIcons(lv_layer_t *layer, const lv_area_t &at) const;
    void RefreshPercent();

    lv_obj_t *face_ = nullptr;
    lv_obj_t *digits_ = nullptr;
    lv_obj_t *icons_ = nullptr;
    lv_obj_t *percent_ = nullptr;
    lv_obj_t *date_ = nullptr;

    ui::ClockView view_ = {};
    bool painted_ = false;  // has `view_` ever been applied

    // Both labels are `lv_label_set_text_static` over these, so a changing
    // string costs no allocation out of LVGL's pool (§10.14.1).
    char percent_text_[8] = {};
    char date_text_[24] = {};
};

}  // namespace screens
