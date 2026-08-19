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
//   * **an entry with nothing behind it is still an entry.** Hiding one would
//     make the list lie about what this device is going to be, so it selects, it
//     answers `kNotBuilt`, and the screen draws it faint with `soon` — which is
//     §10.9's rule about `unknown` being the honest state, applied to a menu.
//     Every row has a screen behind it today; `settings_menu.cpp` says why the
//     mechanism stays anyway;
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
    kWifi = 0,  // → the Wi-Fi screen (§10.8.6)
    kStatus,    // → the status pages (§10.8.5)
    kTouch,     // → the touch test and its calibration (§10.8.5)
    // **The settings file, from the glass** (§10.15). Every setter in this
    // firmware writes to memory and `config save` is what reaches the
    // filesystem — which until now meant that a Wi-Fi mode picked with a finger
    // and a touch correction made with four of them both needed a USB cable to
    // survive a reboot. These two rows are that cable, and the operator's
    // request.
    //
    // Save before reload, by the same rule the two below follow: the further
    // down, the more it costs to press by accident. A save is idempotent; a
    // reload throws away every edit that has not reached the file.
    kConfigSave,
    kConfigReload,
    // The two that *do* something rather than opening something, last and in
    // this order on purpose: the further down the list, the harder it is to
    // undo. A reboot comes back in ten seconds; a power-off comes back when
    // somebody presses the button on the case.
    kReboot,
    kPowerOff,
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
    kArmed,     // the first press on a destructive row: it now asks
    // **One press each, and no arming.** The arming below means exactly one
    // thing — this takes the device away from whoever is looking at it — and
    // neither of these does. What a reload costs is written on the row instead,
    // before anybody presses it, which is the same call §10.8.5 makes about
    // `usb in` on the power-off row.
    kConfigSave,
    kConfigReload,
    kReboot,  // the second press, inside the window
    kPowerOff,
    // **Pressed while the cable is in**, which this chip turns straight back
    // into a power-on (§10.1). Its own answer rather than a silent refusal: the
    // row already says `usb in`, and a press that did nothing and said nothing
    // would read as a device that had stopped listening.
    kPowerOffBlocked,
};

// What the last press on a row that touches the filesystem did. `kNone` is the
// ordinary state of every row, most of the time: nothing was pressed, or what
// was pressed has been on the glass long enough to have been read.
enum class SettingsResult : uint8_t {
    kNone = 0,
    kOk,
    kFailed,
};

class SettingsMenu {
   public:
    static constexpr uint8_t kEntryCount = static_cast<uint8_t>(SettingsEntry::kCount);

    // **Which rows are on the glass is not this file's business, and that is a
    // decision that was taken twice.** The list is longer than the panel — seven
    // rows at the stride `settings_screen.h` draws them at is 624 pixels of
    // 480 — and the first answer here was a window of five that the selection
    // dragged along with it. It worked, and it scrolled *only from the buttons*,
    // which is what the repository owner said about it: a list on a touchscreen
    // is a list you expect to be able to drag.
    //
    // So the scrolling is LVGL's now (`settings_screen.cpp`), and this layer went
    // back to holding one thing: which row is selected. The window, the slot-to-row
    // mapping it needed and the whole class of bug that came with it — a tap on the
    // third *slot* is not a tap on the third *row* — are gone rather than sitting
    // underneath a second mechanism that also scrolls.

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

    // The two rows that take the device away from whoever is looking at it, and
    // therefore the two that ask twice.
    static bool Destructive(SettingsEntry entry);

    // **Whether a power-off would actually happen.** False while USB is in, and
    // the caller is what knows that — this layer has never heard of a PMIC. It is
    // a *state* rather than an argument to `Activate` because the row draws
    // itself with it: the operator should be told the cable is in the way before
    // they press, not after.
    void SetCanPowerOff(bool can) { can_power_off_ = can; }
    bool CanPowerOff() const { return can_power_off_; }

    // How long `saved` / `reloaded` stays on the row. Long enough to be read by
    // somebody who was looking at the row they pressed, short enough that it is
    // gone before it becomes a claim about the present.
    static constexpr uint32_t kResultMs = 3000;

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

    // Is the selected row asking for a second press right now. Takes the clock
    // because the arming expires — asking is a state with a deadline in it, not
    // a flag. Only ever true on a destructive row, because only those arm.
    bool Armed(uint32_t now_ms) const;

    // What the last save or reload did, and when. **The caller is what performs
    // it** — this layer has never heard of a filesystem — and it is a `bool`
    // rather than an `esp_err_t` for the same reason the cable is a `bool`.
    void SetResult(SettingsEntry entry, bool ok, uint32_t now_ms);

    // Is there something to say on this row right now. Takes the clock because
    // the answer expires: a row that still says `saved` ten minutes later is a
    // readout about a moment that has gone.
    SettingsResult Result(SettingsEntry entry, uint32_t now_ms) const;

    // The screen came up. Clears the arming and puts the selection back at the
    // top: coming back into settings and finding the previous visit's armed
    // reboot under a finger is exactly what the arming exists to prevent. The
    // last outcome goes with it, for the same reason — it belonged to that visit.
    void Opened();

   private:
    void Disarm();

    uint8_t selected_ = 0;
    bool armed_ = false;
    bool can_power_off_ = false;
    uint32_t armed_at_ms_ = 0;

    // One outcome, not one per row: two of these cannot happen at once, and a
    // second press replacing the first is what an operator watching one row
    // expects to see.
    SettingsEntry result_row_ = SettingsEntry::kCount;
    bool result_ok_ = false;
    uint32_t result_at_ms_ = 0;
};

}  // namespace ui
