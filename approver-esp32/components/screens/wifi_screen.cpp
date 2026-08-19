#include "wifi_screen.h"

#include <cstdio>
#include <cstring>

namespace screens {
namespace {

// The palette every screen here uses. Nothing new is invented: a Wi-Fi screen
// that looked like a different device would read as somebody else's firmware.
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

const char *RowName(ui::WifiRow row) {
    switch (row) {
        case ui::WifiRow::kMode:
            return "mode";
        case ui::WifiRow::kRecord:
            return "";  // the record row's own text, filled in on every pass
        case ui::WifiRow::kScan:
            return "scan networks";
        case ui::WifiRow::kCount:
            break;
    }
    return "";
}

int32_t RowTop(uint8_t row) {
    // The scan row sits under the two fields rather than next to the other two,
    // because it is what you press *after* reading them.
    if (row == static_cast<uint8_t>(ui::WifiRow::kScan)) {
        return kWifiScanRowTop;
    }
    return kWifiRowTop + row * kWifiRowStride;
}

}  // namespace

esp_err_t WifiScreen::Create(lv_obj_t *parent) {
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

    // **The title is the way out**, a whole row of it — the same call
    // `settings_screen.cpp` makes: `PWR` is the other way back, and a device
    // whose only touch route home is a small chevron is one people press four
    // times.
    lv_obj_t *header = Bare(root_, 0, 0, 480, kWifiTitleHeight);
    if (header == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    lv_obj_add_flag(header, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_user_data(header, this);
    lv_obj_add_event_cb(header, BackClicked, LV_EVENT_CLICKED, nullptr);

    title_ = Text(header, Faint(), kWifiPad, kWifiTitleTop, LV_SYMBOL_LEFT "  wi-fi");
    state_ = Text(header, Faint(), 0, kWifiTitleTop, state_text_);
    if (title_ == nullptr || state_ == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    // Right-aligned against the margin rather than placed, for the reason
    // §10.8.3 gives about its countdowns: this field's width changes with its
    // value, and `connecting` is twice `online`.
    lv_obj_set_width(state_, 260);
    lv_obj_set_style_text_align(state_, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    lv_obj_set_pos(state_, 480 - kWifiPad - 260, kWifiTitleTop);
    lv_obj_set_style_text_font(state_, &lv_font_montserrat_14, LV_PART_MAIN);

    for (uint8_t i = 0; i < ui::WifiView::kRowCount; ++i) {
        Row &row = rows_[i];
        row.plate = Bare(root_, kWifiPad, RowTop(i), 480 - 2 * kWifiPad, kWifiRowHeight);
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

        const auto which = static_cast<ui::WifiRow>(i);
        const bool record = which == ui::WifiRow::kRecord;
        row.label = Text(row.plate, Bright(),
                         record ? kWifiRowTextLeft + 56 : kWifiRowTextLeft, kWifiRowTextTop,
                         record ? record_text_ : RowName(which));
        row.note = Text(row.plate, Dim(), 0, kWifiRowTextTop,
                        which == ui::WifiRow::kMode ? mode_text_ : "");
        if (row.label == nullptr || row.note == nullptr) {
            return ESP_ERR_NO_MEM;
        }
        lv_obj_set_width(row.note, kWifiNoteWidth);
        lv_obj_set_style_text_align(row.note, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
        lv_obj_set_pos(row.note,
                       480 - 2 * kWifiPad - kWifiRowTextLeft - kWifiNoteWidth, kWifiRowTextTop);
    }

    // **The two arrows are their own targets inside the record row.** Wide
    // enough for a fingertip — 6 mm is what §10.8.6 measured a key has to be —
    // and drawn faint when there is nothing to step through, which is the same
    // call `settings_screen.cpp` makes about a row with nothing behind it.
    lv_obj_t *record_plate = rows_[static_cast<uint8_t>(ui::WifiRow::kRecord)].plate;
    left_ = Text(record_plate, Faint(), kWifiRowTextLeft, kWifiRowTextTop, LV_SYMBOL_LEFT);
    right_ = Text(record_plate, Faint(), 0, kWifiRowTextTop, LV_SYMBOL_RIGHT);
    if (left_ == nullptr || right_ == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    lv_obj_set_pos(right_, 480 - 2 * kWifiPad - kWifiRowTextLeft - 24, kWifiRowTextTop);
    lv_obj_t *const arrows[] = {left_, right_};
    for (lv_obj_t *arrow : arrows) {
        // The glyph is about 20 px wide and a fingertip is 8-10 mm — 100 px on
        // this glass (§10.8.6 has the millimetres). The extended click area is
        // what makes the arrow a target rather than a decoration.
        lv_obj_set_ext_click_area(arrow, 26);
        lv_obj_add_flag(arrow, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_user_data(arrow, this);
    }
    lv_obj_add_event_cb(left_, ArrowClicked, LV_EVENT_CLICKED, reinterpret_cast<void *>(1));
    lv_obj_add_event_cb(right_, ArrowClicked, LV_EVENT_CLICKED, reinterpret_cast<void *>(2));

    // The record's two strings. A faint label and a value that wraps rather than
    // being cut — `wifi_screen.h` argues both.
    for (int i = 0; i < 2; ++i) {
        const int32_t y = kWifiFieldTop + i * kWifiFieldStride;
        if (Text(root_, Faint(), kWifiPad, y, i == 0 ? "ssid" : "pass") == nullptr) {
            return ESP_ERR_NO_MEM;
        }
        lv_obj_t *value = Text(root_, Bright(), kWifiValueLeft, y,
                               i == 0 ? ssid_text_ : password_text_);
        if (value == nullptr) {
            return ESP_ERR_NO_MEM;
        }
        lv_obj_set_width(value, kWifiValueWidth);
        lv_label_set_long_mode(value, LV_LABEL_LONG_MODE_WRAP);
        if (i == 0) {
            ssid_ = value;
        } else {
            password_ = value;
        }
    }

    std::snprintf(hint_text_, sizeof hint_text_, "%s", "BOOT next  KEY press  PWR back");
    hint_ = Text(root_, Faint(), kWifiPad, kWifiHintTop, hint_text_);
    if (hint_ == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    lv_obj_set_style_text_font(hint_, &lv_font_montserrat_14, LV_PART_MAIN);

    return ESP_OK;
}

void WifiScreen::SetVisible(bool visible) {
    if (root_ == nullptr || visible == visible_) {
        return;
    }
    visible_ = visible;
    if (visible) {
        lv_obj_remove_flag(root_, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);
        // A screen that is not up must not be holding a tap from the last time it
        // was: acting on it later is acting on an intent from another visit.
        tapped_ = kNoRow;
        arrow_ = 0;
        back_ = false;
    }
}

void WifiScreen::SetValue(lv_obj_t *label, char *cache, size_t size, const char *text) {
    if (std::strncmp(cache, text, size) == 0) {
        return;
    }
    std::snprintf(cache, size, "%s", text);
    // **The small font rather than a cut value** (`wifi_screen.h`): an SSID is
    // 32 bytes and a WPA key 63, and neither fits this column at 28 point. Both
    // sizes are already enabled, so this costs no flash — a *third* size would
    // cost 97 KB, which is the measurement §10.8.3 records.
    lv_obj_set_style_text_font(
        label,
        std::strlen(cache) > kWifiValueBigMax ? &lv_font_montserrat_14 : &lv_font_montserrat_28,
        LV_PART_MAIN);
    lv_label_set_text_static(label, cache);
}

void WifiScreen::Apply(const ui::WifiView &view, const WifiFacts &facts, uint32_t now_ms) {
    if (root_ == nullptr) {
        return;
    }

    const uint8_t selected = static_cast<uint8_t>(view.Selected());
    for (uint8_t i = 0; i < ui::WifiView::kRowCount; ++i) {
        lv_obj_set_style_bg_color(rows_[i].plate, i == selected ? PlateSelected() : Plate(),
                                  LV_PART_MAIN);
    }

    // The mode row.
    const char *mode = ui::WifiModeName(view.Mode());
    if (std::strncmp(mode_text_, mode, sizeof mode_text_) != 0) {
        std::snprintf(mode_text_, sizeof mode_text_, "%s", mode);
        lv_label_set_text_static(rows_[static_cast<uint8_t>(ui::WifiRow::kMode)].note, mode_text_);
    }
    lv_obj_set_style_text_color(rows_[static_cast<uint8_t>(ui::WifiRow::kMode)].note,
                                view.Mode() == ui::WifiMode::kOff ? Dim() : Bright(),
                                LV_PART_MAIN);

    // The record row: which of the records is on the glass, and whether there is
    // anything to step to. **Every one of these three sentences is a different
    // situation** — one access point, one network, or none at all — and a row
    // that spelled them the same way would leave somebody pressing an arrow that
    // was never going to move.
    char wanted[sizeof record_text_] = {};
    if (view.ShowingAp()) {
        std::snprintf(wanted, sizeof wanted, "%s", "access point");
    } else if (view.Count() == 0) {
        std::snprintf(wanted, sizeof wanted, "%s", "no networks");
    } else {
        std::snprintf(wanted, sizeof wanted, "network %u/%u",
                      static_cast<unsigned>(view.Index() + 1),
                      static_cast<unsigned>(view.Count()));
    }
    if (std::strncmp(record_text_, wanted, sizeof record_text_) != 0) {
        std::snprintf(record_text_, sizeof record_text_, "%s", wanted);
        lv_label_set_text_static(rows_[static_cast<uint8_t>(ui::WifiRow::kRecord)].label,
                                 record_text_);
    }
    const lv_color_t arrows = view.CanStep() ? Bright() : Dim();
    lv_obj_set_style_text_color(left_, arrows, LV_PART_MAIN);
    lv_obj_set_style_text_color(right_, arrows, LV_PART_MAIN);
    lv_obj_set_style_text_color(rows_[static_cast<uint8_t>(ui::WifiRow::kRecord)].label,
                                view.Count() == 0 ? Faint() : Bright(), LV_PART_MAIN);

    // The scan row says when there is nowhere for a name to land, before anybody
    // presses it — §10.8.5's rule about the cable being on the power-off row
    // rather than in a refusal.
    lv_obj_t *scan_note = rows_[static_cast<uint8_t>(ui::WifiRow::kScan)].note;
    lv_label_set_text_static(scan_note, view.Count() == 0 ? "no record" : "");
    lv_obj_set_style_text_color(rows_[static_cast<uint8_t>(ui::WifiRow::kScan)].label,
                                view.Count() == 0 ? Faint() : Bright(), LV_PART_MAIN);

    SetValue(ssid_, ssid_text_, sizeof ssid_text_,
             facts.ssid[0] != '\0' ? facts.ssid : "-");
    SetValue(password_, password_text_, sizeof password_text_,
             facts.password[0] != '\0' ? facts.password : "open");

    if (std::strncmp(state_text_, facts.state, sizeof state_text_) != 0) {
        std::snprintf(state_text_, sizeof state_text_, "%s", facts.state);
        lv_label_set_text_static(state_, state_text_);
    }

    // **The hint says the edit is only in memory, for a while, in the place a
    // hint already is.** §10.15's rule is that a screen edits memory and `config
    // save` writes the file, and the operator has to be told once — where they
    // are looking, and not for ever, because a line that will not go away reads
    // as a warning.
    const char *wanted_hint = view.Note(now_ms) ? "in memory - 'config save' keeps it"
                                                : "BOOT next  KEY press  PWR back";
    if (std::strncmp(hint_text_, wanted_hint, sizeof hint_text_) != 0) {
        std::snprintf(hint_text_, sizeof hint_text_, "%s", wanted_hint);
        lv_label_set_text_static(hint_, hint_text_);
    }
}

uint8_t WifiScreen::TakeTap() {
    const uint8_t taken = tapped_;
    tapped_ = kNoRow;
    return taken;
}

int8_t WifiScreen::TakeArrow() {
    const int8_t taken = arrow_;
    arrow_ = 0;
    return taken;
}

bool WifiScreen::TakeBack() {
    const bool taken = back_;
    back_ = false;
    return taken;
}

void WifiScreen::RowClicked(lv_event_t *event) {
    auto *self = static_cast<WifiScreen *>(lv_obj_get_user_data(lv_event_get_target_obj(event)));
    if (self == nullptr) {
        return;
    }
    self->tapped_ = static_cast<uint8_t>(reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)));
}

void WifiScreen::ArrowClicked(lv_event_t *event) {
    auto *self = static_cast<WifiScreen *>(lv_obj_get_user_data(lv_event_get_target_obj(event)));
    if (self == nullptr) {
        return;
    }
    // 1 is the left arrow and 2 the right one, because a user-data pointer of
    // zero cannot be told from one that was never set.
    self->arrow_ = reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)) == 1 ? -1 : 1;
    // An arrow sits inside the record row's plate. LVGL only passes an event to
    // a parent when the child carries `LV_OBJ_FLAG_EVENT_BUBBLE`, which these do
    // not — so the row does not also see this press. Both lines below are there
    // so that adding that flag later cannot quietly make one press step twice.
    self->tapped_ = kNoRow;
    lv_event_stop_bubbling(event);
}

void WifiScreen::BackClicked(lv_event_t *event) {
    auto *self = static_cast<WifiScreen *>(lv_obj_get_user_data(lv_event_get_target_obj(event)));
    if (self != nullptr) {
        self->back_ = true;
    }
}

}  // namespace screens
