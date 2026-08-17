#include "clock_screen.h"

#include <cstdio>
#include <cstring>

#include "esp_log.h"

namespace screens {
namespace {

constexpr const char *TAG = "clock";

// --- The palette ---------------------------------------------------------
// §10.8.1: "black is free, static is expensive". So the background is black and
// nothing on this screen is ever at full scale — the top of the water ramp is a
// vivid green that is still a long way short of `0x00FF00`, which is both harsher
// to look at across a dark room and the fastest way to wear a subpixel out.
//
// The blue channel is not decoration either: a green with a little blue in it
// reads as water rather than as an oscilloscope.
constexpr uint8_t kWaterLowGreen = 56;
constexpr uint8_t kWaterLowBlue = 28;
constexpr uint8_t kWaterHighGreen = 200;
constexpr uint8_t kWaterHighBlue = 100;

lv_color_t Water(uint8_t intensity) {
    const int32_t level = intensity;
    const uint8_t green = static_cast<uint8_t>(
        kWaterLowGreen + (level * (kWaterHighGreen - kWaterLowGreen)) / 255);
    const uint8_t blue = static_cast<uint8_t>(
        kWaterLowBlue + (level * (kWaterHighBlue - kWaterLowBlue)) / 255);
    return lv_color_make(0, green, blue);
}

// The indicators are a fixed brightness rather than part of the water: they are
// read, not looked at, and a status light that shimmers is a status light nobody
// trusts.
lv_color_t Lit() { return lv_color_make(0, 180, 92); }
lv_color_t Hollow() { return lv_color_make(0, 62, 34); }
lv_color_t Shell() { return lv_color_make(112, 112, 112); }
lv_color_t Bright() { return lv_color_make(214, 245, 224); }
lv_color_t BusGreen() { return lv_color_make(0, 190, 96); }
lv_color_t BusRed() { return lv_color_make(198, 44, 32); }
lv_color_t BusIdle() { return lv_color_make(56, 56, 56); }

// The battery's fill, and the two thresholds are the only place this screen
// editorialises: a charge is a number the operator can act on, and a colour is
// how a number gets noticed without being read.
lv_color_t Charge(uint8_t percent) {
    if (percent <= 10) {
        return lv_color_make(198, 44, 32);
    }
    if (percent <= 25) {
        return lv_color_make(206, 148, 0);
    }
    return lv_color_make(0, 176, 88);
}

// --- The seven segments --------------------------------------------------
// Bit per segment, in the order the tables below assume.
enum Segment : uint8_t {
    kTop = 1 << 0,
    kTopRight = 1 << 1,
    kBottomRight = 1 << 2,
    kBottom = 1 << 3,
    kBottomLeft = 1 << 4,
    kTopLeft = 1 << 5,
    kMiddle = 1 << 6,
};

// Which segments each digit lights. Written out rather than computed: a table is
// checkable by eye against the shape it draws, and the arithmetic that would
// replace it is not.
constexpr uint8_t kDigitSegments[10] = {
    /* 0 */ kTop | kTopRight | kBottomRight | kBottom | kBottomLeft | kTopLeft,
    /* 1 */ kTopRight | kBottomRight,
    /* 2 */ kTop | kTopRight | kMiddle | kBottomLeft | kBottom,
    /* 3 */ kTop | kTopRight | kMiddle | kBottomRight | kBottom,
    /* 4 */ kTopLeft | kMiddle | kTopRight | kBottomRight,
    /* 5 */ kTop | kTopLeft | kMiddle | kBottomRight | kBottom,
    /* 6 */ kTop | kTopLeft | kMiddle | kBottomLeft | kBottomRight | kBottom,
    /* 7 */ kTop | kTopRight | kBottomRight,
    /* 8 */ 0x7F,
    /* 9 */ kTop | kTopRight | kMiddle | kTopLeft | kBottomRight | kBottom,
};

// §10.8.2's `--:--`: not a missing glyph, the middle segment on its own.
constexpr uint8_t kDashSegments = kMiddle;

// One segment's box inside a digit, in digit-local coordinates.
struct SegmentBox {
    int32_t x;
    int32_t y;
    int32_t w;
    int32_t h;
    bool vertical;
};

// The vertical run of a segment: the two horizontals at top and bottom, the
// middle one halfway, and the four uprights filling what is left with
// `kSegmentGap` of black at every join.
constexpr int32_t kArmLength = kDigitWidth - 2 * kStroke - 2 * kSegmentGap;
constexpr int32_t kUprightLength = (kDigitHeight - kStroke) / 2 - kStroke - 2 * kSegmentGap;
constexpr int32_t kMiddleY = (kDigitHeight - kStroke) / 2;
constexpr int32_t kLowerY = (kDigitHeight + kStroke) / 2 + kSegmentGap;

constexpr SegmentBox kBoxes[7] = {
    /* kTop         */ {kStroke + kSegmentGap, 0, kArmLength, kStroke, false},
    /* kTopRight    */ {kDigitWidth - kStroke, kStroke + kSegmentGap, kStroke, kUprightLength, true},
    /* kBottomRight */ {kDigitWidth - kStroke, kLowerY, kStroke, kUprightLength, true},
    /* kBottom      */ {kStroke + kSegmentGap, kDigitHeight - kStroke, kArmLength, kStroke, false},
    /* kBottomLeft  */ {0, kLowerY, kStroke, kUprightLength, true},
    /* kTopLeft     */ {0, kStroke + kSegmentGap, kStroke, kUprightLength, true},
    /* kMiddle      */ {kStroke + kSegmentGap, kMiddleY, kArmLength, kStroke, false},
};

static_assert(kUprightLength > 0, "the digit is too short for its own stroke");
static_assert(kArmLength > 0, "the digit is too narrow for its own stroke");

// Where each of the five columns starts, digits-object-local. The colon sits in
// the third slot.
constexpr int32_t kSlotX[5] = {
    0,
    kDigitWidth + kDigitGap,
    2 * (kDigitWidth + kDigitGap),
    2 * (kDigitWidth + kDigitGap) + kColonWidth + kDigitGap,
    3 * (kDigitWidth + kDigitGap) + kColonWidth + kDigitGap,
};

static_assert(kSlotX[4] + kDigitWidth == kDigitsWidth, "the slots do not fill the block");

// A plain object with none of the base widget's decoration: no background, no
// border, no padding, nothing to scroll and nothing to press. Everything on this
// screen is one of these or a label.
lv_obj_t *Bare(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h) {
    lv_obj_t *obj = lv_obj_create(parent);
    if (obj == nullptr) {
        return nullptr;
    }
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_outline_width(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(obj, 0, LV_PART_MAIN);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    return obj;
}

// A filled rounded rectangle in absolute coordinates. Every shape on this screen
// is one of these, which is why it is worth the four lines.
void Fill(lv_layer_t *layer, int32_t x, int32_t y, int32_t w, int32_t h, int32_t radius,
          lv_color_t colour) {
    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_color = colour;
    dsc.bg_opa = LV_OPA_COVER;
    dsc.radius = radius;
    const lv_area_t area = {x, y, x + w - 1, y + h - 1};
    lv_draw_rect(layer, &dsc, &area);
}

// The same, filled with a vertical two-stop gradient. This is what carries the
// water down an upright segment: a flat colour per segment would make the wave
// visible only where segments meet, which reads as a lighting bug rather than as
// a texture. Two stops is all `LV_GRADIENT_MAX_STOPS` offers and all a 52-pixel
// run needs.
void FillGradient(lv_layer_t *layer, int32_t x, int32_t y, int32_t w, int32_t h, int32_t radius,
                  lv_color_t top, lv_color_t bottom) {
    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_opa = LV_OPA_COVER;
    dsc.radius = radius;
    dsc.bg_color = top;
    dsc.bg_grad.dir = LV_GRAD_DIR_VER;
    dsc.bg_grad.stops_count = 2;
    dsc.bg_grad.stops[0].color = top;
    dsc.bg_grad.stops[0].opa = LV_OPA_COVER;
    dsc.bg_grad.stops[0].frac = 0;
    dsc.bg_grad.stops[1].color = bottom;
    dsc.bg_grad.stops[1].opa = LV_OPA_COVER;
    dsc.bg_grad.stops[1].frac = 255;
    const lv_area_t area = {x, y, x + w - 1, y + h - 1};
    lv_draw_rect(layer, &dsc, &area);
}

// An unfilled rounded rectangle — the "not connected" shape of the Wi-Fi bars
// and the battery's shell.
void Outline(lv_layer_t *layer, int32_t x, int32_t y, int32_t w, int32_t h, int32_t radius,
             int32_t width, lv_color_t colour) {
    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_opa = LV_OPA_TRANSP;
    dsc.radius = radius;
    dsc.border_color = colour;
    dsc.border_width = width;
    dsc.border_opa = LV_OPA_COVER;
    dsc.border_side = LV_BORDER_SIDE_FULL;
    const lv_area_t area = {x, y, x + w - 1, y + h - 1};
    lv_draw_rect(layer, &dsc, &area);
}

void Triangle(lv_layer_t *layer, int32_t x1, int32_t y1, int32_t x2, int32_t y2, int32_t x3,
              int32_t y3, lv_color_t colour) {
    lv_draw_triangle_dsc_t dsc;
    lv_draw_triangle_dsc_init(&dsc);
    dsc.color = colour;
    dsc.opa = LV_OPA_COVER;
    dsc.p[0].x = x1;
    dsc.p[0].y = y1;
    dsc.p[1].x = x2;
    dsc.p[1].y = y2;
    dsc.p[2].x = x3;
    dsc.p[2].y = y3;
    lv_draw_triangle(layer, &dsc);
}

}  // namespace

esp_err_t ClockScreen::Create(lv_obj_t *parent) {
    if (parent == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (face_ != nullptr) {
        return ESP_OK;
    }

    // The face is what drifts (§10.8.1): one object moves and everything inside
    // it comes along, so nothing below has to know the panel is an AMOLED.
    face_ = Bare(parent, kRestX, kRestY, kFaceWidth, kFaceHeight);
    if (face_ == nullptr) {
        ESP_LOGE(TAG, "out of LVGL memory building the clock");
        return ESP_ERR_NO_MEM;
    }
    // Children may hang over the edges — the percentage label's line box does —
    // and clipping them would be a silent layout bug rather than a visible one.
    lv_obj_add_flag(face_, LV_OBJ_FLAG_OVERFLOW_VISIBLE);

    icons_ = Bare(face_, 0, 0, kColumnWidth, kPercentTop);
    digits_ = Bare(face_, kColumnWidth + kColumnGap, 0, kDigitsWidth, kDigitHeight);
    if (icons_ == nullptr || digits_ == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    // **Two draw targets rather than one**, and the reason is the water: the
    // digits are repainted ten times a second and the indicators change once a
    // minute, so sharing an object would repaint the battery at 10 Hz for
    // nothing.
    lv_obj_set_user_data(digits_, this);
    lv_obj_set_user_data(icons_, this);
    lv_obj_add_event_cb(digits_, DrawDigits, LV_EVENT_DRAW_MAIN, nullptr);
    lv_obj_add_event_cb(icons_, DrawIcons, LV_EVENT_DRAW_MAIN, nullptr);

    percent_ = lv_label_create(face_);
    date_ = lv_label_create(face_);
    if (percent_ == nullptr || date_ == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    lv_obj_set_pos(percent_, 0, kPercentTop);
    lv_obj_set_style_text_font(percent_, &lv_font_montserrat_28, LV_PART_MAIN);
    lv_obj_set_style_text_color(percent_, Shell(), LV_PART_MAIN);
    lv_label_set_text_static(percent_, percent_text_);

    // The date is under the digits and aligned with them, not with the column:
    // it belongs to the time, and a line that started under the battery would
    // read as being about the battery.
    lv_obj_set_pos(date_, kColumnWidth + kColumnGap, kDateTop);
    lv_obj_set_style_text_font(date_, &lv_font_montserrat_28, LV_PART_MAIN);
    lv_obj_set_style_text_color(date_, Hollow(), LV_PART_MAIN);
    lv_label_set_text_static(date_, date_text_);

    return ESP_OK;
}

void ClockScreen::SetDate(const char *text) {
    if (date_ == nullptr) {
        return;
    }
    const char *wanted = text != nullptr ? text : "";
    if (std::strncmp(date_text_, wanted, sizeof(date_text_)) == 0) {
        return;
    }
    std::snprintf(date_text_, sizeof(date_text_), "%s", wanted);
    // Re-pointed at the same buffer on purpose: `lv_label_set_text_static`
    // re-measures and re-invalidates without allocating, which is the whole
    // reason the buffer is ours (§10.14.1).
    lv_label_set_text_static(date_, date_text_);
}

void ClockScreen::Apply(const ui::ClockView &view) {
    if (face_ == nullptr) {
        return;
    }

    const bool first = !painted_;

    if (first || view.drift_x != view_.drift_x || view.drift_y != view_.drift_y) {
        lv_obj_set_pos(face_, kRestX + view.drift_x, kRestY + view.drift_y);
    }

    const bool digits_moved = first || view.phase != view_.phase ||
                              view.time_valid != view_.time_valid ||
                              std::memcmp(view.digit, view_.digit, sizeof(view.digit)) != 0;

    const bool icons_moved = first || view.wifi != view_.wifi || view.bars != view_.bars ||
                             view.bus != view_.bus || view.battery != view_.battery ||
                             view.battery_known != view_.battery_known ||
                             view.battery_percent != view_.battery_percent;

    view_ = view;
    painted_ = true;

    if (digits_moved) {
        lv_obj_invalidate(digits_);
    }
    if (icons_moved) {
        lv_obj_invalidate(icons_);
        RefreshPercent();
    }
}

void ClockScreen::RefreshPercent() {
    char wanted[sizeof(percent_text_)] = {};
    if (view_.battery_known) {
        std::snprintf(wanted, sizeof(wanted), "%u%%", static_cast<unsigned>(view_.battery_percent));
    } else if (view_.battery == ui::BatteryIcon::kAbsent) {
        // No cell on the connector, which is how this board ships — so the honest
        // line is where the power is coming from, not a percentage of nothing.
        std::snprintf(wanted, sizeof(wanted), "USB");
    } else {
        std::snprintf(wanted, sizeof(wanted), "--");
    }

    if (std::strncmp(percent_text_, wanted, sizeof(percent_text_)) == 0) {
        return;
    }
    std::memcpy(percent_text_, wanted, sizeof(percent_text_));
    lv_label_set_text_static(percent_, percent_text_);

    lv_obj_set_style_text_color(
        percent_, view_.battery_known ? Charge(view_.battery_percent) : Shell(), LV_PART_MAIN);
}

// --- Painting ------------------------------------------------------------

void ClockScreen::DrawDigits(lv_event_t *event) {
    lv_obj_t *obj = lv_event_get_current_target_obj(event);
    auto *self = static_cast<ClockScreen *>(lv_obj_get_user_data(obj));
    if (self == nullptr) {
        return;
    }
    lv_area_t at;
    lv_obj_get_coords(obj, &at);
    self->PaintDigits(lv_event_get_layer(event), at);
}

void ClockScreen::DrawIcons(lv_event_t *event) {
    lv_obj_t *obj = lv_event_get_current_target_obj(event);
    auto *self = static_cast<ClockScreen *>(lv_obj_get_user_data(obj));
    if (self == nullptr) {
        return;
    }
    lv_area_t at;
    lv_obj_get_coords(obj, &at);
    self->PaintIcons(lv_event_get_layer(event), at);
}

void ClockScreen::PaintDigits(lv_layer_t *layer, const lv_area_t &at) const {
    const int32_t radius = kStroke / 2;

    for (int slot = 0; slot < 4; ++slot) {
        const int8_t digit = view_.digit[slot];
        const uint8_t mask = (digit >= 0 && digit <= 9)
                                 ? kDigitSegments[digit]
                                 : kDashSegments;  // §10.8.2: dashes, not midnight

        // The colon lives between slots 1 and 2, so the last two digits are one
        // column further right — `kSlotX` already accounts for it.
        const int32_t base_x = at.x1 + kSlotX[slot < 2 ? slot : slot + 1];

        for (int index = 0; index < 7; ++index) {
            if ((mask & (1u << index)) == 0) {
                // **Unlit segments are not drawn at all.** A ghost segment is
                // the classic look and it is also a permanently lit pixel on a
                // panel that charges for those (§10.8.1).
                continue;
            }
            const SegmentBox &box = kBoxes[index];
            const int32_t x = base_x + box.x;
            const int32_t y = at.y1 + box.y;

            if (box.vertical) {
                // The wave runs down the face, so an upright segment gets it as
                // a gradient between its own two ends.
                FillGradient(layer, x, y, box.w, box.h, radius,
                             Water(ui::ClockFace::Shimmer(static_cast<int16_t>(box.y),
                                                          view_.phase)),
                             Water(ui::ClockFace::Shimmer(
                                 static_cast<int16_t>(box.y + box.h), view_.phase)));
            } else {
                // A horizontal arm is ten pixels tall; one colour is the whole
                // wave it could possibly show.
                Fill(layer, x, y, box.w, box.h, radius,
                     Water(ui::ClockFace::Shimmer(static_cast<int16_t>(box.y + box.h / 2),
                                                  view_.phase)));
            }
        }
    }

    // The colon, and it does not blink: a clock that flashes twice a second is a
    // clock whose brightest pixels never rest, which is the one thing §10.8.1
    // asks this screen not to do.
    const int32_t colon_x = at.x1 + kSlotX[2] + (kColonWidth - kColonDot) / 2;
    const int32_t dots[2] = {
        kDigitHeight / 3 - kColonDot / 2,
        2 * kDigitHeight / 3 - kColonDot / 2,
    };
    for (const int32_t y : dots) {
        Fill(layer, colon_x, at.y1 + y, kColonDot, kColonDot, LV_RADIUS_CIRCLE,
             Water(ui::ClockFace::Shimmer(static_cast<int16_t>(y + kColonDot / 2), view_.phase)));
    }
}

void ClockScreen::PaintIcons(lv_layer_t *layer, const lv_area_t &at) const {
    // --- Wi-Fi: three bars, mobile-fashion ------------------------------
    // The level is bars; the *state* is whether they are filled. An access point
    // and a radio that is off both draw the hollow shape, and the access point
    // puts a mark inside it — because "this device is the network" is not a
    // signal strength and must not be spelled as one.
    constexpr int32_t kBars = ui::ClockFace::kMaxBars;
    constexpr int32_t kBarGap = 5;
    constexpr int32_t kBarWidth = (kWifiWidth - (kBars - 1) * kBarGap) / kBars;
    static_assert(kBarWidth > 0, "the Wi-Fi icon is too narrow for its bars");

    for (int32_t bar = 0; bar < kBars; ++bar) {
        const int32_t height = kWifiHeight * (bar + 2) / (kBars + 1);
        const int32_t x = at.x1 + bar * (kBarWidth + kBarGap);
        const int32_t y = at.y1 + kWifiTop + (kWifiHeight - height);
        if (bar < view_.bars) {
            Fill(layer, x, y, kBarWidth, height, 3, Lit());
        } else {
            Outline(layer, x, y, kBarWidth, height, 3, 2, Hollow());
        }
    }

    if (view_.wifi == ui::WifiIcon::kAp) {
        // A `T` over the hollow bars — the device is the point somebody joins.
        const int32_t centre = at.x1 + kWifiWidth / 2;
        const int32_t top = at.y1 + kWifiTop + 5;
        Fill(layer, centre - 11, top, 22, 6, 3, Lit());
        Fill(layer, centre - 3, top, 6, 24, 3, Lit());
    }

    // --- The bus dot -----------------------------------------------------
    {
        lv_color_t colour = BusIdle();
        switch (view_.bus) {
            case ui::BusIcon::kOff:
                colour = BusIdle();
                break;
            case ui::BusIcon::kDown:
                colour = BusRed();
                break;
            case ui::BusIcon::kUp:
            case ui::BusIcon::kActive:
                colour = BusGreen();
                break;
        }
        const int32_t x = at.x1;
        const int32_t y = at.y1 + kBusTop;
        if (view_.bus == ui::BusIcon::kOff) {
            // Nothing configured is not a fault, so it is not a coloured light:
            // an outline says "there is nothing here to be wrong".
            Outline(layer, x, y, kBusDiameter, kBusDiameter, LV_RADIUS_CIRCLE, 2, colour);
        } else {
            Fill(layer, x, y, kBusDiameter, kBusDiameter, LV_RADIUS_CIRCLE, colour);
        }
        if (view_.bus == ui::BusIcon::kActive) {
            // Green with a dot in it — something arrived in the last two minutes.
            // The dot is a hole rather than a highlight: it is the one shape that
            // cannot be confused with the solid circle next to it, and it lights
            // fewer pixels than a bright one would.
            const int32_t hole = kBusDiameter / 3;
            Fill(layer, x + (kBusDiameter - hole) / 2, y + (kBusDiameter - hole) / 2, hole, hole,
                 LV_RADIUS_CIRCLE, lv_color_black());
        }
    }

    // --- The battery -----------------------------------------------------
    {
        const int32_t x = at.x1;
        const int32_t y = at.y1 + kBatteryTop;
        const int32_t body = kBatteryWidth - kBatteryNub;

        Outline(layer, x, y, body, kBatteryHeight, 4, 2, Shell());
        Fill(layer, x + body, y + kBatteryHeight / 3, kBatteryNub, kBatteryHeight / 3, 1, Shell());

        // Two pixels of border and one of air, so the fill never touches the
        // shell — a bar that reaches the outline reads as full at 90 %.
        const int32_t inset = 4;
        const int32_t track = body - 2 * inset;
        const int32_t height = kBatteryHeight - 2 * inset;

        if (view_.battery_known && view_.battery_percent > 0) {
            int32_t filled = track * view_.battery_percent / 100;
            if (filled < 2) {
                filled = 2;  // a charge that exists is visible, however small
            }
            Fill(layer, x + inset, y + inset, filled, height, 2, Charge(view_.battery_percent));
        }

        const bool powered = view_.battery == ui::BatteryIcon::kCharging ||
                            view_.battery == ui::BatteryIcon::kExternal ||
                            view_.battery == ui::BatteryIcon::kAbsent;
        if (powered) {
            // A bolt, in a colour that shows against the fill and against the
            // empty shell alike — the two backgrounds it has to work over.
            const int32_t cx = x + body / 2;
            const int32_t cy = y + kBatteryHeight / 2;
            const int32_t top = y + inset;
            const int32_t bottom = y + kBatteryHeight - inset;
            const lv_color_t colour =
                view_.battery == ui::BatteryIcon::kCharging ? Bright() : Shell();
            Triangle(layer, cx + 4, top, cx - 7, cy + 2, cx + 3, cy + 2, colour);
            Triangle(layer, cx - 4, bottom, cx + 7, cy - 2, cx - 3, cy - 2, colour);
        }
    }
}

}  // namespace screens
