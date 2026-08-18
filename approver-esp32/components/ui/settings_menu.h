#pragma once

// The settings list, decided where it can be tested (CLAUDE.md §10.8.5, §10.11).
//
// **This file includes `<cstdint>` and nothing else**, which by now is the shape
// of this firmware rather than a choice made per screen — the navigator, the
// clock face, the request card, the limits, the Wi-Fi policy, the internet
// check, the sync schedule and the bus link all manage it. What lives here is
// which row is selected, which rows have something behind them yet, and what a
// press on each one means; `screens/settings_screen.cpp` turns that into
// widgets and has no rules in it.
//
// Two things this owns that are not obvious:
//
//   * **an entry with nothing behind it is still an entry.** Wi-Fi and the touch
//     calibration are on the list and are not built, and hiding them would make
//     the list lie about what this device is going to be. They select, they
//     answer `kNotBuilt`, and the screen says so — which is §10.9's rule about
//     `unknown` being the honest state, applied to a menu;
//   * **reboot is armed before it fires.** §10.7 argues that the *console's*
//     `reboot` needs no confirmation, because a reboot undoes itself in seconds
//     and a second word would be friction on the most ordinary debugging action
//     there is. A touchscreen is the other case: nobody typed anything, a stray
//     finger on a desk object is a real event, and §10.8.5 already makes its
//     destructive entries two-step. So the first press arms and the second one
//     goes — and the arming expires on its own, because an armed reboot left
//     sitting on the glass is the stray finger with extra steps.

#include <cstdint>

namespace ui {

// The rows, in the order they are drawn. **The order is the operator's**, from
// the repository owner: the two settings screens, then the one thing that is a
// test rather than a setting, then — last, and last on purpose — reboot.
//
// The gap §10.8.5 leaves for "a few more, we will know later" is deliberately
// **not** here: a row that does nothing and says nothing is worse than a short
// list, and adding one later costs a line.
enum class SettingsEntry : uint8_t {
    kWifi = 0,  // → the Wi-Fi screen (§10.8.6), not built
    kStatus,    // → the status pages (§10.8.5)
    kTouch,     // → touch calibration and test, not built
    kReboot,    // the only row that does something rather than opening something
    kCount,
};

// What a press produced. `kNone` is a press on nothing, which happens when a tap
// lands outside every row.
enum class SettingsAction : uint8_t {
    kNone = 0,
    kOpenWifi,
    kOpenStatus,
    kOpenTouch,
    kNotBuilt,  // a row that is on the list and has no screen behind it yet
    kArmed,     // the first press on reboot: it now asks
    kReboot,    // the second press, inside the window
};

class SettingsMenu {
   public:
    static constexpr uint8_t kEntryCount = static_cast<uint8_t>(SettingsEntry::kCount);

    // How long an armed reboot stays armed. Long enough to move a finger from
    // one press to a second deliberate one, short enough that walking away
    // leaves the device safe — which is the whole reason it expires at all.
    static constexpr uint32_t kArmedMs = 5000;

    SettingsMenu() = default;
    SettingsMenu(const SettingsMenu &) = delete;
    SettingsMenu &operator=(const SettingsMenu &) = delete;

    // Is there a screen behind this row today. Static because it is a fact about
    // the firmware rather than about this menu's state, and because the screen
    // needs it to draw the row faint before anybody presses anything.
    static bool Built(SettingsEntry entry);

    uint8_t Selected() const { return selected_; }
    SettingsEntry SelectedEntry() const { return static_cast<SettingsEntry>(selected_); }

    // The next row, wrapping. **Unbuilt rows are not skipped**: a selection that
    // jumps over what it is pointing at is a selection nobody can predict, and
    // the row itself is what explains why it does nothing.
    void Next();

    // Point at a row — what a tap does. Out of range is ignored rather than
    // clamped: a tap that landed between rows selected nothing, and moving the
    // selection to whichever end happened to be nearest is a guess.
    void Select(uint8_t index);

    // Act on the selected row.
    SettingsAction Activate(uint32_t now_ms);

    // Is the reboot row asking for a second press right now. Takes the clock
    // because the arming expires — asking is a state with a deadline in it, not
    // a flag.
    bool RebootArmed(uint32_t now_ms) const;

    // The screen came up. Clears the arming and puts the selection back at the
    // top: coming back into settings and finding the previous visit's armed
    // reboot under a finger is exactly what the arming exists to prevent.
    void Opened();

   private:
    void Disarm();

    uint8_t selected_ = 0;
    bool armed_ = false;
    uint32_t armed_at_ms_ = 0;
};

}  // namespace ui
