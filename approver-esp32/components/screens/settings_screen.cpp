#include "settings_screen.h"

#include <cstdio>
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
        case ui::SettingsEntry::kConfigSave:
            return "config save";
        case ui::SettingsEntry::kConfigReload:
            return "config reload";
        case ui::SettingsEntry::kReboot:
            return "reboot";
        case ui::SettingsEntry::kPowerOff:
            return "power off";
        case ui::SettingsEntry::kCount:
            break;
    }
    return "";
}

// What a row says when nothing has happened on it. **Only one row has anything
// to say**, and it is the row whose cost is not obvious: a reload throws away
// every edit that has not reached the file, and `settings_menu.cpp` gives that
// warning as the reason the row needs no arming. Written before it is pressed,
// which is the same call §10.8.5 makes about `usb in`.
const char *RowHint(ui::SettingsEntry entry) {
    // **Two words, and the board is what chose the number.** `drops unsaved` was
    // thirteen characters, which at Montserrat 28 is about 190 px against a note
    // column that starts 180 px into a 432 px plate — so on the panel it shared
    // pixels with `config reload`, which is 200 px of label. §10.8.5 already
    // records this exact finding about the status page's label column and §10.8.6
    // about the scan row; this is the third time, and the answer is the same one:
    // measure it on the glass, then shorten the words rather than widening the box.
    return entry == ui::SettingsEntry::kConfigReload ? "edits lost" : "";
}

// And what it says afterwards. A save that reached the filesystem and one that
// did not are the same press from the glass otherwise — the console's `config`
// readout is not on the panel.
const char *ResultText(ui::SettingsEntry entry, ui::SettingsResult result) {
    switch (result) {
        case ui::SettingsResult::kOk:
            return entry == ui::SettingsEntry::kConfigSave ? "saved" : "reloaded";
        case ui::SettingsResult::kFailed:
            return "failed";
        case ui::SettingsResult::kNone:
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

    // **Where in the list the eye is**, because five rows of seven is a screen
    // that would otherwise not say the other two exist. Right-aligned against the
    // same margin the rows use, for the reason §10.8.3 gives about its countdowns:
    // the field's width changes with its value.
    std::snprintf(position_text_, sizeof position_text_, "1 / %u",
                  static_cast<unsigned>(ui::SettingsMenu::kEntryCount));
    position_ = Text(title_, Dim(), 0, kSettingsTitleTop, position_text_);
    if (position_ == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    lv_obj_set_width(position_, kSettingsNoteWidth);
    lv_obj_set_style_text_align(position_, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    lv_obj_set_pos(position_, 480 - kSettingsPad - kSettingsNoteWidth, kSettingsTitleTop);

    // **The scroll container, and the four lines that make a list draggable.**
    // Vertical only, because there is nothing beside a row to reach; a scrollbar
    // on `AUTO`, because five rows of seven is otherwise a screen that does not
    // admit the other two exist; and the elastic/momentum flags LVGL puts on by
    // default are kept, since they are what makes a drag feel like a list rather
    // than like a slider.
    //
    // **What it costs is the swipe that used to leave this screen.** LVGL
    // suppresses a gesture while something is scrolling (`indev_gesture` returns
    // early on `scroll_obj`), so a vertical drag here is scrolling and nothing
    // else — the way out with a finger is the title above, or a swipe *sideways*,
    // which no scroll consumes. `PWR` is unchanged.
    list_ = Bare(root_, 0, kSettingsListTop, 480, kSettingsListHeight);
    if (list_ == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    lv_obj_add_flag(list_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(list_, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list_, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_pad_bottom(list_, kSettingsListPadBottom, LV_PART_MAIN);

    for (uint8_t i = 0; i < ui::SettingsMenu::kEntryCount; ++i) {
        MenuRow &row = rows_[i];
        const auto entry = static_cast<ui::SettingsEntry>(i);

        row.plate = Bare(list_, kSettingsPad, i * kSettingsRowStride,
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

        // A row's name never changes again, so it is written here and the label
        // holds a literal — which is what a row per entry buys back.
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
        // **Coming back into settings starts at the top**, which is
        // `SettingsMenu::Opened`'s rule for the selection and has to be the scroll
        // as well: the menu puts the highlight on row one, and a list still
        // scrolled where it was left would show it nowhere. No animation — this is
        // the screen appearing rather than moving.
        if (list_ != nullptr) {
            lv_obj_scroll_to_y(list_, 0, LV_ANIM_OFF);
            scrolled_rows_ = 0;
        }
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

    // **Sampled before the early return below, because a finger can scroll this
    // list without changing one thing the menu knows about.** It is read here
    // rather than in `screens::Menu()` for the ordinary reason: this runs under the
    // display lock and that does not.
    if (list_ != nullptr) {
        const int32_t scrolled = lv_obj_get_scroll_y(list_);
        const int32_t rows = scrolled > 0 ? scrolled / kSettingsRowStride : 0;
        scrolled_rows_ = static_cast<uint8_t>(
            rows > ui::SettingsMenu::kEntryCount ? ui::SettingsMenu::kEntryCount : rows);
    }

    const uint8_t selected = menu.Selected();
    const bool armed = menu.Armed(now_ms);
    const bool can_power_off = menu.CanPowerOff();
    // Which row has something to say about what just happened to it, asked once
    // for the whole screen: `Result` takes the clock, so the answer changes on
    // its own when the note's window closes and a repaint is what takes it off.
    ui::SettingsResult result = ui::SettingsResult::kNone;
    uint8_t result_row = kNoRow;
    for (uint8_t i = 0; i < ui::SettingsMenu::kEntryCount; ++i) {
        const ui::SettingsResult on_row =
            menu.Result(static_cast<ui::SettingsEntry>(i), now_ms);
        if (on_row != ui::SettingsResult::kNone) {
            result = on_row;
            result_row = i;
            break;  // there is only ever one (`settings_menu.h` says why)
        }
    }

    if (selected == shown_selected_ && armed == shown_armed_ &&
        can_power_off == shown_can_power_off_ && result == shown_result_ &&
        result_row == shown_result_row_) {
        return;
    }
    const bool selection_moved = selected != shown_selected_;
    shown_selected_ = selected;
    shown_armed_ = armed;
    shown_can_power_off_ = can_power_off;
    shown_result_ = result;
    shown_result_row_ = result_row;

    std::snprintf(position_text_, sizeof position_text_, "%u / %u",
                  static_cast<unsigned>(selected + 1),
                  static_cast<unsigned>(ui::SettingsMenu::kEntryCount));
    lv_label_set_text_static(position_, position_text_);

    for (uint8_t i = 0; i < ui::SettingsMenu::kEntryCount; ++i) {
        MenuRow &row = rows_[i];
        const auto entry = static_cast<ui::SettingsEntry>(i);
        const bool is_selected = i == selected;
        lv_obj_set_style_bg_color(row.plate, is_selected ? PlateSelected() : Plate(),
                                  LV_PART_MAIN);

        // **The rows whose words change**, and they change colour with them: an
        // amber row that asks a question is not the same object as a grey row
        // that opens a screen.
        //
        // The power-off says why it cannot happen *before* anybody presses it.
        // §10.1: VBUS is a power-on source, so a shutdown with the cable in is
        // one the hardware undoes — the honest thing is to put the cable on the
        // row rather than to answer a press with a refusal.
        const bool asking = armed && is_selected && ui::SettingsMenu::Destructive(entry);
        const bool blocked = entry == ui::SettingsEntry::kPowerOff && !can_power_off;
        const bool reporting = i == result_row;

        const char *note = RowHint(entry);
        if (asking) {
            note = "press again";
        } else if (blocked) {
            note = "usb in";
        } else if (reporting) {
            note = ResultText(entry, result);
        }
        lv_label_set_text_static(row.note, note);

        lv_color_t label_colour = Bright();
        if (asking) {
            label_colour = Attention();
        } else if (blocked) {
            label_colour = Faint();
        }
        lv_obj_set_style_text_color(row.label, label_colour, LV_PART_MAIN);

        lv_color_t note_colour = Dim();
        if (asking || (reporting && result == ui::SettingsResult::kFailed)) {
            note_colour = Attention();
        } else if (reporting) {
            note_colour = Bright();
        }
        lv_obj_set_style_text_color(row.note, note_colour, LV_PART_MAIN);
    }

    // **The button route and the finger route move the same list**, which is the
    // whole of what replaced the window: `BOOT` steps the selection and this is
    // what brings it onto the glass. Only when it *moved* — scrolling the list to
    // the selection on every repaint would fight the finger that just dragged it
    // somewhere else.
    if (selection_moved && list_ != nullptr) {
        lv_obj_scroll_to_view(rows_[selected].plate, LV_ANIM_ON);
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
