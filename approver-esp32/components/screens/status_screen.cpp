#include "status_screen.h"

#include <cstdio>
#include <cstring>

namespace screens {
namespace {

lv_color_t Bright() { return lv_color_make(214, 222, 216); }
lv_color_t Faint() { return lv_color_make(122, 130, 126); }

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

}  // namespace

esp_err_t StatusScreen::Create(lv_obj_t *parent) {
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

    // **The body is the page-turn and the header is the way back**, which is the
    // whole touch surface: two targets, each half the size of a hand, and no way
    // to hit one while aiming at the other. `BOOT` pages it too, for the operator
    // who is holding the thing rather than tapping it.
    lv_obj_t *header = Bare(root_, 0, 0, 480, kStatusFirstRowTop - 12);
    body_ = Bare(root_, 0, kStatusFirstRowTop - 12, 480, 480 - (kStatusFirstRowTop - 12));
    if (header == nullptr || body_ == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    lv_obj_add_flag(header, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_user_data(header, this);
    lv_obj_add_event_cb(header, BackClicked, LV_EVENT_CLICKED, nullptr);

    lv_obj_add_flag(body_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_user_data(body_, this);
    lv_obj_add_event_cb(body_, BodyClicked, LV_EVENT_CLICKED, nullptr);

    std::snprintf(title_text_, sizeof title_text_, "%s", LV_SYMBOL_LEFT "  status");
    title_ = Text(header, Faint(), kStatusPad, kStatusTitleTop, title_text_);
    page_ = Text(header, Faint(), 0, kStatusTitleTop, page_text_);
    if (title_ == nullptr || page_ == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    lv_obj_set_width(page_, 200);
    lv_obj_set_style_text_align(page_, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    lv_obj_set_pos(page_, 480 - kStatusPad - 200, kStatusTitleTop);

    for (uint8_t i = 0; i < kStatusRows; ++i) {
        const int32_t y = (kStatusFirstRowTop - (kStatusFirstRowTop - 12)) + i * kStatusRowStride;
        label_[i] = Text(body_, Faint(), kStatusPad, y, "");
        value_[i] = Text(body_, Bright(), kStatusValueLeft, y, value_text_[i]);
        if (label_[i] == nullptr || value_[i] == nullptr) {
            return ESP_ERR_NO_MEM;
        }
        // **The label column is bounded, and that is a bug fix rather than
        // tidiness.** A label wider than the gap simply drew over the value —
        // `magnitude` did, on the board, and what was on the glass was two words
        // sharing the same pixels. A clip makes the same mistake *visible* as a
        // cut-off word, which is the difference between a layout somebody fixes
        // and a number somebody misreads.
        lv_obj_set_width(label_[i], kStatusValueLeft - kStatusPad - 8);
        lv_label_set_long_mode(label_[i], LV_LABEL_LONG_MODE_CLIP);
    }

    return ESP_OK;
}

void StatusScreen::SetVisible(bool visible) {
    if (root_ == nullptr || visible == visible_) {
        return;
    }
    visible_ = visible;
    if (visible) {
        lv_obj_remove_flag(root_, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);
        next_ = false;
        back_ = false;
    }
}

void StatusScreen::Apply(const StatusFacts &facts) {
    if (root_ == nullptr) {
        return;
    }

    char wanted_page[sizeof page_text_] = {};
    std::snprintf(wanted_page, sizeof wanted_page, "%s %u/%u", facts.title,
                  static_cast<unsigned>(facts.page + 1),
                  static_cast<unsigned>(facts.page_count));
    if (std::strncmp(page_text_, wanted_page, sizeof page_text_) != 0) {
        std::snprintf(page_text_, sizeof page_text_, "%s", wanted_page);
        lv_label_set_text_static(page_, page_text_);
    }

    for (uint8_t i = 0; i < kStatusRows; ++i) {
        const bool used = i < facts.rows;
        const char *label = used && facts.label[i] != nullptr ? facts.label[i] : "";
        const char *value = used ? facts.value[i] : "";

        // The label is a literal from the caller and never changes for a given
        // page, so re-pointing at it is free; the value is copied, because the
        // caller's `StatusFacts` is a local somewhere.
        if (lv_label_get_text(label_[i]) != label) {
            lv_label_set_text_static(label_[i], label);
        }
        if (std::strncmp(value_text_[i], value, kStatusValueSize) != 0) {
            std::snprintf(value_text_[i], kStatusValueSize, "%s", value);
            lv_label_set_text_static(value_[i], value_text_[i]);
        }
    }
}

bool StatusScreen::TakeNext() {
    const bool taken = next_;
    next_ = false;
    return taken;
}

bool StatusScreen::TakeBack() {
    const bool taken = back_;
    back_ = false;
    return taken;
}

void StatusScreen::BodyClicked(lv_event_t *event) {
    auto *self = static_cast<StatusScreen *>(lv_obj_get_user_data(lv_event_get_target_obj(event)));
    if (self != nullptr) {
        self->next_ = true;
    }
}

void StatusScreen::BackClicked(lv_event_t *event) {
    auto *self = static_cast<StatusScreen *>(lv_obj_get_user_data(lv_event_get_target_obj(event)));
    if (self != nullptr) {
        self->back_ = true;
    }
}

}  // namespace screens
