#include "settings_screen.h"

#include <cstring>

namespace screens {
namespace {

// The palette the other screens use. Nothing new is invented here: a settings
// list that looked like a different device would be a settings list that reads
// as somebody else's firmware.
lv_color_t Bright() { return lv_color_make(214, 222, 216); }
lv_color_t Faint() { return lv_color_make(122, 130, 126); }
lv_color_t Dim() { return lv_color_make(78, 84, 81); }
lv_color_t Plate() { return lv_color_make(26, 30, 28); }
lv_color_t PlateSelected() { return lv_color_make(44, 62, 52); }
lv_color_t Attention() { return lv_color_make(214, 158, 46); }

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

lv_obj_t *Text(lv_obj_t *parent, lv_color_t colour, int32_t x, int32_t y, const char *literal) {
    lv_obj_t *label = lv_label_create(parent);
    if (label == nullptr) {
        return nullptr;
    }
    lv_obj_set_pos(label, x, y);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_28, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, colour, LV_PART_MAIN);
    lv_label_set_text_static(label, literal);
    return label;
}

// The words on the rows. Literals rather than buffers: none of them changes, and
// `lv_label_set_text_static` over a literal costs nothing out of LVGL's pool.
const char *RowName(ui::SettingsEntry entry) {
    switch (entry) {
        case ui::SettingsEntry::kWifi:
            return "wi-fi";
        case ui::SettingsEntry::kStatus:
            return "status";
        case ui::SettingsEntry::kTouch:
            return "touch test";
        case ui::SettingsEntry::kReboot:
            return "reboot";
        case ui::SettingsEntry::kPowerOff:
            return "power off";
        case ui::SettingsEntry::kCount:
            break;
    }
    return "";
}

}  // namespace

esp_err_t SettingsScreen::Create(lv_obj_t *parent) {
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

    // **The title is the way out**, and it is a whole row of a target rather
    // than a chevron: `PWR` is the other way back, and a device where the only
    // touch route home is a 30-pixel arrow is a device people press four times.
    title_ = Bare(root_, 0, 0, 480, kSettingsFirstRowTop);
    if (title_ == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    lv_obj_add_flag(title_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_user_data(title_, this);
    lv_obj_add_event_cb(title_, BackClicked, LV_EVENT_CLICKED, nullptr);
    if (Text(title_, Faint(), kSettingsPad, kSettingsTitleTop, LV_SYMBOL_LEFT "  settings") ==
        nullptr) {
        return ESP_ERR_NO_MEM;
    }

    for (uint8_t i = 0; i < ui::SettingsMenu::kEntryCount; ++i) {
        const auto entry = static_cast<ui::SettingsEntry>(i);
        MenuRow &row = rows_[i];

        row.plate = Bare(root_, kSettingsPad, kSettingsFirstRowTop + i * kSettingsRowStride,
                         480 - 2 * kSettingsPad, kSettingsRowHeight);
        if (row.plate == nullptr) {
            return ESP_ERR_NO_MEM;
        }
        lv_obj_set_style_bg_opa(row.plate, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_bg_color(row.plate, Plate(), LV_PART_MAIN);
        lv_obj_set_style_radius(row.plate, 10, LV_PART_MAIN);
        lv_obj_add_flag(row.plate, LV_OBJ_FLAG_CLICKABLE);
        // The row index travels as the user data of its own plate, so the
        // callback needs no table to look anything up in — and `this` is found
        // through the parent, which is the one place it can be.
        lv_obj_set_user_data(row.plate, this);
        lv_obj_add_event_cb(row.plate, RowClicked, LV_EVENT_CLICKED,
                            reinterpret_cast<void *>(static_cast<uintptr_t>(i)));

        row.label = Text(row.plate, Bright(), kSettingsRowTextLeft, kSettingsRowTextTop,
                         RowName(entry));
        row.note = Text(row.plate, Dim(), 0, kSettingsRowTextTop, "");
        if (row.label == nullptr || row.note == nullptr) {
            return ESP_ERR_NO_MEM;
        }
        lv_obj_set_width(row.note, kSettingsNoteWidth);
        lv_obj_set_style_text_align(row.note, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
        lv_obj_set_pos(row.note, 480 - 2 * kSettingsPad - kSettingsRowTextLeft -
                                     kSettingsNoteWidth,
                       kSettingsRowTextTop);

        // **A row with nothing behind it says so before it is pressed.** §10.9's
        // rule that `unknown` is the honest state: a list that looked complete
        // and then did nothing would be a list nobody presses twice.
        if (!ui::SettingsMenu::Built(entry)) {
            lv_obj_set_style_text_color(row.label, Faint(), LV_PART_MAIN);
            lv_label_set_text_static(row.note, "soon");
        }
    }

    return ESP_OK;
}

void SettingsScreen::SetVisible(bool visible) {
    if (root_ == nullptr || visible == visible_) {
        return;
    }
    visible_ = visible;
    if (visible) {
        lv_obj_remove_flag(root_, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);
        // A screen that is not up must not be holding a tap from the last time
        // it was: acting on it later is acting on an intent from another visit.
        tapped_ = kNoRow;
        back_ = false;
    }
}

void SettingsScreen::Apply(const ui::SettingsMenu &menu, uint32_t now_ms) {
    if (root_ == nullptr) {
        return;
    }

    const uint8_t selected = menu.Selected();
    const bool armed = menu.Armed(now_ms);
    const bool can_power_off = menu.CanPowerOff();
    if (selected == shown_selected_ && armed == shown_armed_ &&
        can_power_off == shown_can_power_off_) {
        return;
    }
    shown_selected_ = selected;
    shown_armed_ = armed;
    shown_can_power_off_ = can_power_off;

    for (uint8_t i = 0; i < ui::SettingsMenu::kEntryCount; ++i) {
        MenuRow &row = rows_[i];
        const auto entry = static_cast<ui::SettingsEntry>(i);
        lv_obj_set_style_bg_color(row.plate, i == selected ? PlateSelected() : Plate(),
                                  LV_PART_MAIN);

        if (!ui::SettingsMenu::Destructive(entry)) {
            continue;
        }
        // **The two rows whose words change**, and they change colour with them:
        // an amber row that asks a question is not the same object as a grey row
        // that opens a screen.
        //
        // And the power-off says why it cannot happen *before* anybody presses
        // it. §10.1: VBUS is a power-on source, so a shutdown with the cable in
        // is one the hardware undoes — the honest thing is to put the cable on
        // the row rather than to answer a press with a refusal.
        const bool asking = armed && i == selected;
        const bool blocked = entry == ui::SettingsEntry::kPowerOff && !can_power_off;

        const char *note = "";
        if (asking) {
            note = "press again";
        } else if (blocked) {
            note = "usb in";
        }
        lv_label_set_text_static(row.note, note);
        lv_obj_set_style_text_color(row.label,
                                    asking ? Attention() : (blocked ? Faint() : Bright()),
                                    LV_PART_MAIN);
        lv_obj_set_style_text_color(row.note, asking ? Attention() : Dim(), LV_PART_MAIN);
    }
}

uint8_t SettingsScreen::TakeTap() {
    const uint8_t taken = tapped_;
    tapped_ = kNoRow;
    return taken;
}

bool SettingsScreen::TakeBack() {
    const bool taken = back_;
    back_ = false;
    return taken;
}

void SettingsScreen::RowClicked(lv_event_t *event) {
    auto *self = static_cast<SettingsScreen *>(lv_obj_get_user_data(lv_event_get_target_obj(event)));
    if (self == nullptr) {
        return;
    }
    const auto row = static_cast<uint8_t>(
        reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)));
    self->tapped_ = row;
}

void SettingsScreen::BackClicked(lv_event_t *event) {
    auto *self = static_cast<SettingsScreen *>(lv_obj_get_user_data(lv_event_get_target_obj(event)));
    if (self == nullptr) {
        return;
    }
    self->back_ = true;
}

}  // namespace screens
