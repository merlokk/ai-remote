#include "touch_screen.h"

#include <cstdio>
#include <cstring>

namespace screens {
namespace {

lv_color_t Bright() { return lv_color_make(214, 222, 216); }
lv_color_t Faint() { return lv_color_make(122, 130, 126); }
lv_color_t Finger() { return lv_color_make(0, 190, 96); }
lv_color_t Cross() { return lv_color_make(214, 158, 46); }
lv_color_t Bad() { return lv_color_make(190, 56, 44); }

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
    // **Never clickable, on any object on this screen.** `touch_screen.h` says
    // why: the buttons are the escape hatch precisely because the glass may be
    // the broken thing.
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    return obj;
}

lv_obj_t *Text(lv_obj_t *parent, lv_color_t colour, int32_t x, int32_t y, const char *buffer) {
    lv_obj_t *label = lv_label_create(parent);
    if (label == nullptr) {
        return nullptr;
    }
    lv_obj_set_pos(label, x, y);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_28, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, colour, LV_PART_MAIN);
    lv_label_set_text_static(label, buffer);
    return label;
}

void Bar(lv_layer_t *layer, const lv_area_t &at, lv_color_t colour, int32_t x, int32_t y,
         int32_t w, int32_t h) {
    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_color = colour;
    dsc.bg_opa = LV_OPA_COVER;
    dsc.radius = 0;

    lv_area_t area;
    area.x1 = at.x1 + x;
    area.y1 = at.y1 + y;
    area.x2 = area.x1 + w - 1;
    area.y2 = area.y1 + h - 1;
    lv_draw_rect(layer, &dsc, &area);
}

// A cross, drawn as two bars. Two bars rather than a glyph for the reason
// §10.8.2 draws its digits as segments: a shape is a shape, and a font that
// exists only to hold one is tens of kilobytes of flash.
void Crosshair(lv_layer_t *layer, const lv_area_t &at, lv_color_t colour, int32_t cx, int32_t cy) {
    Bar(layer, at, colour, cx - kTouchArm, cy - kTouchStroke / 2, 2 * kTouchArm, kTouchStroke);
    Bar(layer, at, colour, cx - kTouchStroke / 2, cy - kTouchArm, kTouchStroke, 2 * kTouchArm);
}

}  // namespace

esp_err_t TouchScreen::Create(lv_obj_t *parent) {
    if (parent == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (root_ != nullptr) {
        return ESP_OK;
    }

    root_ = Bare(parent, 0, 0, 480, 480);
    if (root_ == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    lv_obj_set_style_bg_color(root_, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(root_, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);

    // The whole panel is one draw target: the crosshair moves with the finger,
    // so anything smaller would be a rectangle that has to be recomputed as
    // often as the thing inside it moves.
    canvas_ = Bare(root_, 0, 0, 480, 480);
    if (canvas_ == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    lv_obj_set_user_data(canvas_, this);
    lv_obj_add_event_cb(canvas_, DrawMarks, LV_EVENT_DRAW_MAIN, nullptr);

    std::snprintf(title_text_, sizeof title_text_, "%s", "touch test");
    title_ = Text(root_, Faint(), kTouchPad, kTouchTitleTop, title_text_);
    if (title_ == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    for (int i = 0; i < 3; ++i) {
        line_[i] = Text(root_, i == 0 ? Bright() : Faint(), kTouchPad,
                        kTouchReadoutTop + i * kTouchReadoutStride, line_text_[i]);
        if (line_[i] == nullptr) {
            return ESP_ERR_NO_MEM;
        }
    }

    std::snprintf(hint_text_, sizeof hint_text_, "%s", "BOOT calibrate  KEY reset  PWR back");
    hint_ = Text(root_, Faint(), kTouchPad, 480 - 46, hint_text_);
    if (hint_ == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    lv_obj_set_style_text_font(hint_, &lv_font_montserrat_14, LV_PART_MAIN);

    return ESP_OK;
}

void TouchScreen::SetVisible(bool visible) {
    if (root_ == nullptr) {
        return;
    }
    if (visible) {
        lv_obj_remove_flag(root_, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);
    }
}

void TouchScreen::Apply(const TouchView &view) {
    if (root_ == nullptr) {
        return;
    }

    const bool marks_moved = view.stage != view_.stage || view.touching != view_.touching ||
                             view.screen_x != view_.screen_x || view.screen_y != view_.screen_y ||
                             view.target_x != view_.target_x || view.target_y != view_.target_y;
    view_ = view;

    char title[sizeof title_text_] = {};
    char first[sizeof line_text_[0]] = {};
    char second[sizeof line_text_[0]] = {};
    char third[sizeof line_text_[0]] = {};

    switch (view.stage) {
        case ui::TouchStage::kTest:
            std::snprintf(title, sizeof title, "touch test");
            if (view.touching) {
                std::snprintf(first, sizeof first, "screen  %u, %u",
                              static_cast<unsigned>(view.screen_x),
                              static_cast<unsigned>(view.screen_y));
                std::snprintf(second, sizeof second, "raw     %u, %u",
                              static_cast<unsigned>(view.raw_x),
                              static_cast<unsigned>(view.raw_y));
            } else {
                std::snprintf(first, sizeof first, "press the glass");
                std::snprintf(second, sizeof second, " ");
            }
            // **The correction in use, always on screen.** "Is this device
            // calibrated" is the question this screen is opened to answer, and
            // an empty line would be an answer to a different one.
            if (view.calibration.Identity()) {
                std::snprintf(third, sizeof third, "no correction");
            } else {
                std::snprintf(third, sizeof third, "x %d/1000%+d  y %d/1000%+d",
                              static_cast<int>(view.calibration.scale_x),
                              static_cast<int>(view.calibration.offset_x),
                              static_cast<int>(view.calibration.scale_y),
                              static_cast<int>(view.calibration.offset_y));
            }
            break;

        case ui::TouchStage::kCollecting:
            std::snprintf(title, sizeof title, "calibrate  %u of %u",
                          static_cast<unsigned>(view.collected + 1),
                          static_cast<unsigned>(ui::kTouchTargets));
            std::snprintf(first, sizeof first, "press the cross");
            std::snprintf(second, sizeof second, " ");
            std::snprintf(third, sizeof third, "PWR to give up");
            break;

        case ui::TouchStage::kResult:
            std::snprintf(title, sizeof title, "calibrate");
            std::snprintf(first, sizeof first, "%s", ui::TouchFitText(view.outcome));
            std::snprintf(second, sizeof second, " ");
            if (view.outcome == ui::TouchFit::kOk) {
                std::snprintf(third, sizeof third, "'config save' keeps it");
            } else {
                std::snprintf(third, sizeof third, "nothing was changed");
            }
            break;
    }

    if (std::strncmp(title_text_, title, sizeof title_text_) != 0) {
        std::snprintf(title_text_, sizeof title_text_, "%s", title);
        lv_label_set_text_static(title_, title_text_);
    }

    const char *wanted[3] = {first, second, third};
    for (int i = 0; i < 3; ++i) {
        if (std::strncmp(line_text_[i], wanted[i], sizeof line_text_[0]) != 0) {
            std::snprintf(line_text_[i], sizeof line_text_[0], "%s", wanted[i]);
            lv_label_set_text_static(line_[i], line_text_[i]);
        }
    }
    // A refusal is the one line on this screen that is red: it is the only thing
    // here the operator has to act on.
    lv_obj_set_style_text_color(
        line_[0],
        view.stage == ui::TouchStage::kResult && view.outcome != ui::TouchFit::kOk ? Bad()
                                                                                   : Bright(),
        LV_PART_MAIN);

    if (marks_moved) {
        lv_obj_invalidate(canvas_);
    }
}

void TouchScreen::DrawMarks(lv_event_t *event) {
    auto *self = static_cast<TouchScreen *>(lv_obj_get_user_data(lv_event_get_target_obj(event)));
    if (self == nullptr) {
        return;
    }
    lv_layer_t *layer = lv_event_get_layer(event);
    lv_area_t area;
    lv_obj_get_coords(lv_event_get_target_obj(event), &area);
    self->Paint(layer, area);
}

void TouchScreen::Paint(lv_layer_t *layer, const lv_area_t &at) const {
    if (view_.stage == ui::TouchStage::kCollecting) {
        Crosshair(layer, at, Cross(), view_.target_x, view_.target_y);
    }
    // The finger is drawn in every stage it is known in, including over a cross:
    // seeing the two apart is the whole of what the operator is being asked to
    // correct.
    if (view_.touching) {
        Crosshair(layer, at, Finger(), view_.screen_x, view_.screen_y);
    }
}

}  // namespace screens
