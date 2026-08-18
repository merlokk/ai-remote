#include "settings_menu.h"

namespace ui {

bool SettingsMenu::Built(SettingsEntry entry) {
    switch (entry) {
        case SettingsEntry::kStatus:
        case SettingsEntry::kTouch:
        case SettingsEntry::kReboot:
            return true;
        case SettingsEntry::kWifi:
        case SettingsEntry::kCount:
            break;
    }
    return false;
}

void SettingsMenu::Next() {
    // Moving off the reboot row disarms it. Without this, arming it, wandering
    // down the list and coming back would leave one press between a stray finger
    // and a restart — the arming would be a delay rather than a confirmation.
    Disarm();
    selected_ = static_cast<uint8_t>((selected_ + 1) % kEntryCount);
}

void SettingsMenu::Select(uint8_t index) {
    if (index >= kEntryCount) {
        return;
    }
    if (index != selected_) {
        Disarm();
    }
    selected_ = index;
}

SettingsAction SettingsMenu::Activate(uint32_t now_ms) {
    switch (SelectedEntry()) {
        case SettingsEntry::kStatus:
            return SettingsAction::kOpenStatus;

        case SettingsEntry::kWifi:
            // The action the row *will* produce is named even though nothing
            // consumes it yet, so that building the screen is a change to the
            // caller rather than a change here.
            return Built(SettingsEntry::kWifi) ? SettingsAction::kOpenWifi
                                               : SettingsAction::kNotBuilt;

        case SettingsEntry::kTouch:
            return Built(SettingsEntry::kTouch) ? SettingsAction::kOpenTouch
                                                : SettingsAction::kNotBuilt;

        case SettingsEntry::kReboot:
            if (RebootArmed(now_ms)) {
                Disarm();
                return SettingsAction::kReboot;
            }
            armed_ = true;
            armed_at_ms_ = now_ms;
            return SettingsAction::kArmed;

        case SettingsEntry::kCount:
            break;
    }
    return SettingsAction::kNone;
}

bool SettingsMenu::RebootArmed(uint32_t now_ms) const {
    // Subtraction rather than a comparison of two absolute counters, so the
    // ~49-day wrap is arithmetic and not a case — every other window in this
    // firmware is written the same way, and this is the one where getting it
    // wrong reboots a device somebody was reading.
    return armed_ && (now_ms - armed_at_ms_) < kArmedMs;
}

void SettingsMenu::Opened() {
    Disarm();
    selected_ = 0;
}

void SettingsMenu::Disarm() {
    armed_ = false;
    armed_at_ms_ = 0;
}

}  // namespace ui
