#include "wifi_scan_screen.h"

#include <cstdio>
#include <cstring>

namespace screens {
namespace {

lv_color_t Bright() { return lv_color_make(214, 222, 216); }
lv_color_t Faint() { return lv_color_make(122, 130, 126); }
lv_color_t Dim() { return lv_color_make(78, 84, 81); }
lv_color_t Plate() { return lv_color_make(26, 30, 28); }
lv_color_t PlateSelected() { return lv_color_make(44, 62, 52); }

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

// What the header says instead of a count. **Four answers rather than two**: a
// scan in flight, an empty flat, a radio that refused, and a list.
const char *Headline(ui::WifiScanState state, uint8_t count, char *out, size_t size) {
    switch (state) {
        case ui::WifiScanState::kScanning:
            return "looking...";
        case ui::WifiScanState::kEmpty:
            return "nothing on the air";
        case ui::WifiScanState::kFailed:
            return "the radio refused";
        case ui::WifiScanState::kList:
            std::snprintf(out, size, "%u found", static_cast<unsigned>(count));
            return out;
        case ui::WifiScanState::kIdle:
            break;
    }
    return "";
}

}  // namespace

esp_err_t WifiScanScreen::Create(lv_obj_t *parent) {
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

    lv_obj_t *header = Bare(root_, 0, 0, 480, kScanTitleHeight);
    if (header == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    lv_obj_add_flag(header, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_user_data(header, this);
    lv_obj_add_event_cb(header, BackClicked, LV_EVENT_CLICKED, nullptr);

    title_ = Text(header, Faint(), kScanPad, kScanTitleTop, LV_SYMBOL_LEFT "  networks");
    count_ = Text(header, Faint(), 0, kScanTitleTop, count_text_);
    if (title_ == nullptr || count_ == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    lv_obj_set_width(count_, 260);
    lv_obj_set_style_text_align(count_, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    lv_obj_set_pos(count_, 480 - kScanPad - 260, kScanTitleTop);
    lv_obj_set_style_text_font(count_, &lv_font_montserrat_14, LV_PART_MAIN);

    for (uint8_t i = 0; i < ui::kWifiScanRows; ++i) {
        Row &row = rows_[i];
        row.plate = Bare(root_, kScanPad, kScanRowTop + i * kScanRowStride,
                         480 - 2 * kScanPad, kScanRowHeight);
        if (row.plate == nullptr) {
            return ESP_ERR_NO_MEM;
        }
        lv_obj_set_style_bg_opa(row.plate, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_bg_color(row.plate, Plate(), LV_PART_MAIN);
        lv_obj_set_style_radius(row.plate, 10, LV_PART_MAIN);
        lv_obj_add_flag(row.plate, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_user_data(row.plate, this);
        lv_obj_add_event_cb(row.plate, RowClicked, LV_EVENT_CLICKED,
                            reinterpret_cast<void *>(static_cast<uintptr_t>(i)));

        row.label = Text(row.plate, Bright(), kScanRowTextLeft, kScanRowTextTop, row_text_[i]);
        row.note = Text(row.plate, Dim(), 0, kScanRowTextTop, note_text_[i]);
        if (row.label == nullptr || row.note == nullptr) {
            return ESP_ERR_NO_MEM;
        }
        // **The name is clipped rather than allowed to draw over the signal.**
        // §10.8.5 found that on the status page the hard way: a label wider than
        // its gap drew two words in the same pixels, and a clip turns the same
        // mistake into a cut-off word somebody can see.
        lv_obj_set_width(row.label,
                         480 - 2 * kScanPad - kScanRowTextLeft - kScanNoteWidth - 8);
        lv_label_set_long_mode(row.label, LV_LABEL_LONG_MODE_CLIP);

        lv_obj_set_width(row.note, kScanNoteWidth);
        lv_obj_set_style_text_align(row.note, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
        lv_obj_set_pos(row.note, 480 - 2 * kScanPad - kScanRowTextLeft - kScanNoteWidth,
                       kScanRowTextTop);
        lv_obj_set_style_text_font(row.note, &lv_font_montserrat_14, LV_PART_MAIN);
    }

    hint_ = Text(root_, Faint(), kScanPad, kScanHintTop, "BOOT next  KEY pick  PWR back");
    if (hint_ == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    lv_obj_set_style_text_font(hint_, &lv_font_montserrat_14, LV_PART_MAIN);

    return ESP_OK;
}

void WifiScanScreen::SetVisible(bool visible) {
    if (root_ == nullptr || visible == visible_) {
        return;
    }
    visible_ = visible;
    if (visible) {
        lv_obj_remove_flag(root_, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);
        tapped_ = kNoRow;
        back_ = false;
    }
}

void WifiScanScreen::Apply(const ui::WifiView &view) {
    if (root_ == nullptr) {
        return;
    }

    char scratch[sizeof count_text_] = {};
    const char *headline = Headline(view.ScanState(), view.ScanCount(), scratch, sizeof scratch);
    if (std::strncmp(count_text_, headline, sizeof count_text_) != 0) {
        std::snprintf(count_text_, sizeof count_text_, "%s", headline);
        lv_label_set_text_static(count_, count_text_);
    }

    for (uint8_t i = 0; i < ui::kWifiScanRows; ++i) {
        const uint8_t absolute = static_cast<uint8_t>(view.ScanWindow() + i);
        const ui::WifiScanEntry *entry = view.ScanEntry(absolute);
        Row &row = rows_[i];

        // A row with no network on it is **empty and unlit**, not a plate with
        // nothing in it: on an AMOLED an unlit pixel costs nothing (§10.8.1), and
        // a visible plate would look like a name that failed to draw.
        lv_obj_set_style_bg_opa(row.plate, entry != nullptr ? LV_OPA_COVER : LV_OPA_TRANSP,
                                LV_PART_MAIN);
        lv_obj_set_style_bg_color(row.plate,
                                  absolute == view.ScanSelected() ? PlateSelected() : Plate(),
                                  LV_PART_MAIN);

        const char *name = entry != nullptr ? entry->ssid : "";
        if (std::strncmp(row_text_[i], name, sizeof row_text_[i]) != 0) {
            std::snprintf(row_text_[i], sizeof row_text_[i], "%s", name);
            lv_label_set_text_static(row.label, row_text_[i]);
        }

        char note[sizeof note_text_[i]] = {};
        if (entry != nullptr) {
            std::snprintf(note, sizeof note, "%d  %s", static_cast<int>(entry->rssi),
                          entry->secured ? "wpa" : "open");
        }
        if (std::strncmp(note_text_[i], note, sizeof note) != 0) {
            std::snprintf(note_text_[i], sizeof note_text_[i], "%s", note);
            lv_label_set_text_static(row.note, note_text_[i]);
        }
    }
}

uint8_t WifiScanScreen::TakeTap() {
    const uint8_t taken = tapped_;
    tapped_ = kNoRow;
    return taken;
}

bool WifiScanScreen::TakeBack() {
    const bool taken = back_;
    back_ = false;
    return taken;
}

void WifiScanScreen::RowClicked(lv_event_t *event) {
    auto *self =
        static_cast<WifiScanScreen *>(lv_obj_get_user_data(lv_event_get_target_obj(event)));
    if (self == nullptr) {
        return;
    }
    self->tapped_ = static_cast<uint8_t>(reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)));
}

void WifiScanScreen::BackClicked(lv_event_t *event) {
    auto *self =
        static_cast<WifiScanScreen *>(lv_obj_get_user_data(lv_event_get_target_obj(event)));
    if (self != nullptr) {
        self->back_ = true;
    }
}

}  // namespace screens
