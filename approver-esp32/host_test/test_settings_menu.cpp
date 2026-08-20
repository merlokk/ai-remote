// The settings list and the status pager (CLAUDE.md §10.8.5), tested where they
// cost nothing to test (§10.11, host tier).
//
// Both headers include `<cstdint>` and nothing else, so this suite needs no fake
// — the navigator's shape rather than the drivers'.
//
// Nearly all of the value is in one place: **reboot is armed before it fires**,
// and every way of getting a single press to reach a restart is a test here.
// Everything else on this screen opens something; that row is the only one that
// takes the device away from the operator while they are looking at it.

#include "settings_menu.h"
#include "status_pages.h"
#include "unity.h"

using ui::SettingsAction;
using ui::SettingsEntry;
using ui::SettingsMenu;
using ui::SettingsResult;
using ui::StatusPage;
using ui::StatusPager;

namespace {

constexpr uint8_t kWifi = static_cast<uint8_t>(SettingsEntry::kWifi);
constexpr uint8_t kStatus = static_cast<uint8_t>(SettingsEntry::kStatus);
constexpr uint8_t kTouch = static_cast<uint8_t>(SettingsEntry::kTouch);
constexpr uint8_t kSave = static_cast<uint8_t>(SettingsEntry::kConfigSave);
constexpr uint8_t kReload = static_cast<uint8_t>(SettingsEntry::kConfigReload);
constexpr uint8_t kReboot = static_cast<uint8_t>(SettingsEntry::kReboot);
constexpr uint8_t kPowerOff = static_cast<uint8_t>(SettingsEntry::kPowerOff);

// A menu with the cable out, which is the only state a power-off can happen in.
void Unplugged(SettingsMenu *menu) { menu->SetCanPowerOff(true); }

}  // namespace

// --- The list ------------------------------------------------------------

void test_the_menu_opens_at_the_top(void) {
    SettingsMenu menu;
    TEST_ASSERT_EQUAL_UINT8(0, menu.Selected());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(SettingsEntry::kWifi),
                          static_cast<int>(menu.SelectedEntry()));
}

void test_the_two_destructive_rows_are_last_and_in_order(void) {
    // The owner's order, and the part of it that is a safety property rather than
    // a preference: the rows that take the device away are as far as possible
    // from the one a finger lands on when the screen opens — and the one that is
    // hardest to undo is last of all. A reboot comes back in ten seconds; a
    // power-off comes back when somebody presses the button on the case.
    TEST_ASSERT_EQUAL_UINT8(SettingsMenu::kEntryCount - 1, kPowerOff);
    TEST_ASSERT_EQUAL_UINT8(SettingsMenu::kEntryCount - 2, kReboot);
    TEST_ASSERT_TRUE(SettingsMenu::Destructive(SettingsEntry::kReboot));
    TEST_ASSERT_TRUE(SettingsMenu::Destructive(SettingsEntry::kPowerOff));
    TEST_ASSERT_FALSE(SettingsMenu::Destructive(SettingsEntry::kStatus));
    TEST_ASSERT_FALSE(SettingsMenu::Destructive(SettingsEntry::kWifi));
    TEST_ASSERT_FALSE(SettingsMenu::Destructive(SettingsEntry::kTouch));
}

void test_power_off_is_blocked_while_the_cable_is_in(void) {
    // §10.1: VBUS is a power-on source for this chip, so a shutdown with the
    // cable in is one the hardware immediately undoes — and what the operator
    // would see is not a device switching off but a device rebooting. The
    // console's `poweroff` already refuses for this reason; the row does too.
    SettingsMenu menu;  // the default is "cannot", which is a device on a cable
    menu.Select(kPowerOff);
    TEST_ASSERT_FALSE(menu.CanPowerOff());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(SettingsAction::kPowerOffBlocked),
                          static_cast<int>(menu.Activate(1000)));
}

void test_a_blocked_power_off_does_not_arm(void) {
    // **Refused before it is armed, not after.** Otherwise the first press would
    // ask the operator to confirm something the hardware is going to refuse, and
    // the second press would be the one that finally explained why — and worse,
    // unplugging the cable at that moment would leave a device one stray press
    // from switching off.
    SettingsMenu menu;
    menu.Select(kPowerOff);
    menu.Activate(1000);
    TEST_ASSERT_FALSE(menu.Armed(1000));

    Unplugged(&menu);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(SettingsAction::kArmed),
                          static_cast<int>(menu.Activate(1100)));
}

void test_power_off_takes_two_presses_like_reboot(void) {
    SettingsMenu menu;
    Unplugged(&menu);
    menu.Select(kPowerOff);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(SettingsAction::kArmed),
                          static_cast<int>(menu.Activate(1000)));
    TEST_ASSERT_TRUE(menu.Armed(1000));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(SettingsAction::kPowerOff),
                          static_cast<int>(menu.Activate(1000 + SettingsMenu::kArmedMs - 1)));
}

void test_an_armed_power_off_expires_like_an_armed_reboot(void) {
    SettingsMenu menu;
    Unplugged(&menu);
    menu.Select(kPowerOff);
    menu.Activate(1000);
    TEST_ASSERT_FALSE(menu.Armed(1000 + SettingsMenu::kArmedMs));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(SettingsAction::kArmed),
                          static_cast<int>(menu.Activate(1000 + SettingsMenu::kArmedMs)));
}

void test_arming_one_destructive_row_does_not_arm_the_other(void) {
    // They share one flag, so this is the test that says the flag belongs to the
    // *selected* row: arming the reboot and then stepping onto power-off must not
    // leave the second one one press from firing.
    SettingsMenu menu;
    Unplugged(&menu);
    menu.Select(kReboot);
    menu.Activate(1000);
    menu.Select(kPowerOff);
    TEST_ASSERT_FALSE(menu.Armed(1100));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(SettingsAction::kArmed),
                          static_cast<int>(menu.Activate(1100)));

    // And the other way round.
    SettingsMenu back;
    Unplugged(&back);
    back.Select(kPowerOff);
    back.Activate(1000);
    back.Select(kReboot);
    TEST_ASSERT_FALSE(back.Armed(1100));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(SettingsAction::kArmed),
                          static_cast<int>(back.Activate(1100)));
}

void test_next_walks_every_row_and_wraps(void) {
    SettingsMenu menu;
    for (uint8_t i = 1; i < SettingsMenu::kEntryCount; ++i) {
        menu.Next();
        TEST_ASSERT_EQUAL_UINT8(i, menu.Selected());
    }
    menu.Next();
    TEST_ASSERT_EQUAL_UINT8(0, menu.Selected());
}

void test_a_tap_between_rows_selects_nothing(void) {
    SettingsMenu menu;
    menu.Select(kStatus);
    menu.Select(SettingsMenu::kEntryCount);      // one past the end
    menu.Select(SettingsMenu::kEntryCount + 40);  // and well past it
    TEST_ASSERT_EQUAL_UINT8(kStatus, menu.Selected());
}

// --- What a press means --------------------------------------------------

void test_the_status_row_opens_the_status(void) {
    SettingsMenu menu;
    menu.Select(kStatus);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(SettingsAction::kOpenStatus),
                          static_cast<int>(menu.Activate(1000)));
}

void test_the_wifi_row_opens_the_wifi_screen(void) {
    // It was the last row on the list to get a screen (§10.8.6), and the reason
    // this layer no longer has a word for a row without one.
    SettingsMenu menu;
    menu.Select(kWifi);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(SettingsAction::kOpenWifi),
                          static_cast<int>(menu.Activate(1000)));
}

void test_the_touch_row_opens_the_touch_test(void) {
    SettingsMenu menu;
    menu.Select(kTouch);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(SettingsAction::kOpenTouch),
                          static_cast<int>(menu.Activate(1000)));
}

// --- Reboot, which is the whole reason this file is worth having ---------

void test_one_press_on_reboot_does_not_reboot(void) {
    SettingsMenu menu;
    menu.Select(kReboot);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(SettingsAction::kArmed),
                          static_cast<int>(menu.Activate(1000)));
    TEST_ASSERT_TRUE(menu.Armed(1000));
}

void test_a_second_press_inside_the_window_reboots(void) {
    SettingsMenu menu;
    menu.Select(kReboot);
    menu.Activate(1000);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(SettingsAction::kReboot),
                          static_cast<int>(menu.Activate(1000 + SettingsMenu::kArmedMs - 1)));
}

void test_the_arming_expires_and_the_next_press_arms_again(void) {
    // The test this row exists for: an armed reboot left on the glass must not
    // still be one press away when somebody comes back to the desk.
    SettingsMenu menu;
    menu.Select(kReboot);
    menu.Activate(1000);

    TEST_ASSERT_FALSE(menu.Armed(1000 + SettingsMenu::kArmedMs));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(SettingsAction::kArmed),
                          static_cast<int>(menu.Activate(1000 + SettingsMenu::kArmedMs)));
}

void test_moving_off_the_reboot_row_disarms_it(void) {
    // Otherwise the arming is a delay rather than a confirmation: arm it, wander
    // down the list, come back, and one press restarts the device.
    SettingsMenu armed_by_next;
    armed_by_next.Select(kReboot);
    armed_by_next.Activate(1000);
    armed_by_next.Next();  // wraps to the top
    armed_by_next.Select(kReboot);
    TEST_ASSERT_FALSE(armed_by_next.Armed(1100));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(SettingsAction::kArmed),
                          static_cast<int>(armed_by_next.Activate(1100)));

    SettingsMenu armed_by_tap;
    armed_by_tap.Select(kReboot);
    armed_by_tap.Activate(1000);
    armed_by_tap.Select(kStatus);
    armed_by_tap.Select(kReboot);
    TEST_ASSERT_FALSE(armed_by_tap.Armed(1100));
}

void test_walking_the_list_round_with_the_button_disarms_the_reboot(void) {
    // **The button route, which is not the tap route**, and the mutation pass is
    // what found it missing: `BOOT` steps the selection with `Next` and never
    // calls `Select`, so a list walked all the way round comes back to the reboot
    // row by a path that the tap test above does not cover. Without the disarm in
    // `Next`, arming it, pressing `BOOT` four times and pressing `KEY` restarts
    // the device on what the operator reads as a first press.
    SettingsMenu menu;
    menu.Select(kReboot);
    menu.Activate(1000);

    for (uint8_t i = 0; i < SettingsMenu::kEntryCount; ++i) {
        menu.Next();
    }
    TEST_ASSERT_EQUAL_UINT8(kReboot, menu.Selected());
    TEST_ASSERT_FALSE(menu.Armed(1100));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(SettingsAction::kArmed),
                          static_cast<int>(menu.Activate(1100)));
}

void test_selecting_the_reboot_row_again_does_not_disarm_it(void) {
    // The mirror image, and it is what makes a tap-tap on the same row work: a
    // touch reports both the selection and the press, so re-selecting the row
    // that is already selected must leave the arming alone.
    SettingsMenu menu;
    menu.Select(kReboot);
    menu.Activate(1000);
    menu.Select(kReboot);
    TEST_ASSERT_TRUE(menu.Armed(1100));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(SettingsAction::kReboot),
                          static_cast<int>(menu.Activate(1100)));
}

void test_leaving_and_coming_back_disarms_and_goes_to_the_top(void) {
    SettingsMenu menu;
    menu.Select(kReboot);
    menu.Activate(1000);
    menu.Opened();
    TEST_ASSERT_EQUAL_UINT8(0, menu.Selected());
    TEST_ASSERT_FALSE(menu.Armed(1100));
}

void test_the_arming_window_survives_the_millisecond_wrap(void) {
    // Both sides of it: a window written `now < armed_at + window` passes every
    // check taken after the counter wraps and fails only in the moments just
    // before, which is where an unexpected reboot would come from.
    SettingsMenu menu;
    menu.Select(kReboot);
    menu.Activate(0xFFFFFF00u);

    TEST_ASSERT_TRUE(menu.Armed(0xFFFFFF10u));  // 16 ms later, not yet wrapped
    TEST_ASSERT_TRUE(menu.Armed(400));          // wrapped, 656 ms after
    TEST_ASSERT_FALSE(menu.Armed(0xFFFFFF00u + SettingsMenu::kArmedMs));
}

// --- The two config rows -------------------------------------------------
//
// One press each, and no arming — which is the decision worth a test rather
// than the code behind it. The arming of the two rows below them means exactly
// one thing: *this takes the device away from whoever is looking at it*. Neither
// of these does — a save is idempotent, and a reload is one press away from
// being redone — so what a reload costs is said on the row instead, permanently,
// rather than asked about after the fact.

void test_the_config_rows_sit_between_the_screens_and_the_destructive_ones(void) {
    // §10.8.5's order: the further down the list, the harder to undo. These two
    // touch a file; the two under them end the session.
    TEST_ASSERT_EQUAL_UINT8(kTouch + 1, kSave);
    TEST_ASSERT_EQUAL_UINT8(kSave + 1, kReload);
    TEST_ASSERT_EQUAL_UINT8(kReload + 1, kReboot);
    TEST_ASSERT_FALSE(SettingsMenu::Destructive(SettingsEntry::kConfigSave));
    TEST_ASSERT_FALSE(SettingsMenu::Destructive(SettingsEntry::kConfigReload));
}

void test_a_save_is_one_press(void) {
    SettingsMenu menu;
    menu.Select(kSave);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(SettingsAction::kConfigSave),
                          static_cast<int>(menu.Activate(1000)));
    TEST_ASSERT_FALSE(menu.Armed(1000));
}

void test_a_reload_is_one_press(void) {
    SettingsMenu menu;
    menu.Select(kReload);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(SettingsAction::kConfigReload),
                          static_cast<int>(menu.Activate(1000)));
    TEST_ASSERT_FALSE(menu.Armed(1000));
}

// --- The list is longer than the panel, and LVGL is what scrolls it -------
//
// The window that used to be here — five slots, a selection that dragged them
// along, and a tap that had to be mapped from a slot to a row — is gone, at the
// repository owner's request: it scrolled from the buttons only, and a list on a
// touchscreen is a list you expect to drag. `settings_screen.cpp` makes the rows a
// scroll container and LVGL does the rest, so what is left to test here is the one
// thing this layer still owns, which is the selection.
//
// The two properties that mattered did not disappear, they moved: that the
// selected row is *on the glass* is now `lv_obj_scroll_to_view`'s job, and that a
// tap lands on the row under the finger is now true by construction, because every
// row is its own widget carrying its own index.

void test_the_selection_is_the_only_thing_this_layer_scrolls(void) {
    // It still walks all seven and wraps — which is what `BOOT` does, and the
    // screen is what brings the selection into view afterwards.
    SettingsMenu menu;
    for (uint8_t i = 1; i < SettingsMenu::kEntryCount; ++i) {
        menu.Next();
        TEST_ASSERT_EQUAL_UINT8(i, menu.Selected());
    }
    menu.Next();
    TEST_ASSERT_EQUAL_UINT8(0, menu.Selected());
}

void test_reopening_the_list_goes_back_to_the_first_row(void) {
    SettingsMenu menu;
    for (uint8_t i = 0; i < SettingsMenu::kEntryCount - 1; ++i) {
        menu.Next();
    }
    TEST_ASSERT_NOT_EQUAL_UINT8(0, menu.Selected());
    menu.Opened();
    TEST_ASSERT_EQUAL_UINT8(0, menu.Selected());
}

// --- What happened, on the row it happened on ----------------------------
//
// A save that reached the filesystem and one that did not are the same press
// from the glass otherwise: the row is where the operator is looking, and
// §10.15's console readout is not.

void test_a_save_says_so_for_a_while(void) {
    SettingsMenu menu;
    menu.SetResult(SettingsEntry::kConfigSave, true, 1000);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(SettingsResult::kOk),
                          static_cast<int>(menu.Result(SettingsEntry::kConfigSave, 1000)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(SettingsResult::kOk),
                          static_cast<int>(menu.Result(SettingsEntry::kConfigSave,
                                                       1000 + SettingsMenu::kResultMs - 1)));
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(SettingsResult::kNone),
        static_cast<int>(menu.Result(SettingsEntry::kConfigSave,
                                     1000 + SettingsMenu::kResultMs)));
}

void test_a_failure_is_its_own_word(void) {
    SettingsMenu menu;
    menu.SetResult(SettingsEntry::kConfigSave, false, 1000);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(SettingsResult::kFailed),
                          static_cast<int>(menu.Result(SettingsEntry::kConfigSave, 1200)));
}

void test_the_result_belongs_to_one_row(void) {
    SettingsMenu menu;
    menu.SetResult(SettingsEntry::kConfigReload, true, 1000);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(SettingsResult::kOk),
                          static_cast<int>(menu.Result(SettingsEntry::kConfigReload, 1000)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(SettingsResult::kNone),
                          static_cast<int>(menu.Result(SettingsEntry::kConfigSave, 1000)));
}

void test_a_second_outcome_replaces_the_first(void) {
    SettingsMenu menu;
    menu.SetResult(SettingsEntry::kConfigSave, true, 1000);
    menu.SetResult(SettingsEntry::kConfigReload, false, 1500);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(SettingsResult::kNone),
                          static_cast<int>(menu.Result(SettingsEntry::kConfigSave, 1500)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(SettingsResult::kFailed),
                          static_cast<int>(menu.Result(SettingsEntry::kConfigReload, 1500)));
}

void test_the_saved_note_survives_the_millisecond_wrap(void) {
    SettingsMenu menu;
    menu.SetResult(SettingsEntry::kConfigSave, true, 0xFFFFFF00u);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(SettingsResult::kOk),
                          static_cast<int>(menu.Result(SettingsEntry::kConfigSave, 0xFFFFFFF0u)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(SettingsResult::kOk),
                          static_cast<int>(menu.Result(SettingsEntry::kConfigSave, 400)));
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(SettingsResult::kNone),
        static_cast<int>(menu.Result(SettingsEntry::kConfigSave,
                                     0xFFFFFF00u + SettingsMenu::kResultMs)));
}

void test_leaving_the_screen_drops_the_result(void) {
    // It belongs to the visit it happened in. Coming back to `saved` on a row
    // nobody has pressed this time is a readout about a moment that has gone.
    SettingsMenu menu;
    menu.SetResult(SettingsEntry::kConfigSave, true, 1000);
    menu.Opened();
    TEST_ASSERT_EQUAL_INT(static_cast<int>(SettingsResult::kNone),
                          static_cast<int>(menu.Result(SettingsEntry::kConfigSave, 1000)));
}

// --- The pager -----------------------------------------------------------

void test_the_status_opens_on_the_first_page(void) {
    StatusPager pager;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(StatusPage::kPower), static_cast<int>(pager.Page()));
}

void test_the_pages_wrap(void) {
    StatusPager pager;
    for (uint8_t i = 1; i < StatusPager::kPageCount; ++i) {
        pager.Next();
        TEST_ASSERT_EQUAL_UINT8(i, pager.Index());
    }
    pager.Next();
    TEST_ASSERT_EQUAL_UINT8(0, pager.Index());
}

void test_reopening_the_status_goes_back_to_the_first_page(void) {
    StatusPager pager;
    pager.Next();
    pager.Reset();
    TEST_ASSERT_EQUAL_UINT8(0, pager.Index());
}

void RegisterSettingsMenuTests(void) {
    RUN_TEST(test_the_menu_opens_at_the_top);
    RUN_TEST(test_the_two_destructive_rows_are_last_and_in_order);
    RUN_TEST(test_next_walks_every_row_and_wraps);
    RUN_TEST(test_a_tap_between_rows_selects_nothing);

    RUN_TEST(test_the_status_row_opens_the_status);
    RUN_TEST(test_the_wifi_row_opens_the_wifi_screen);
    RUN_TEST(test_the_touch_row_opens_the_touch_test);

    RUN_TEST(test_one_press_on_reboot_does_not_reboot);
    RUN_TEST(test_a_second_press_inside_the_window_reboots);
    RUN_TEST(test_the_arming_expires_and_the_next_press_arms_again);
    RUN_TEST(test_moving_off_the_reboot_row_disarms_it);
    RUN_TEST(test_walking_the_list_round_with_the_button_disarms_the_reboot);
    RUN_TEST(test_selecting_the_reboot_row_again_does_not_disarm_it);
    RUN_TEST(test_leaving_and_coming_back_disarms_and_goes_to_the_top);
    RUN_TEST(test_the_arming_window_survives_the_millisecond_wrap);

    RUN_TEST(test_power_off_is_blocked_while_the_cable_is_in);
    RUN_TEST(test_a_blocked_power_off_does_not_arm);
    RUN_TEST(test_power_off_takes_two_presses_like_reboot);
    RUN_TEST(test_an_armed_power_off_expires_like_an_armed_reboot);
    RUN_TEST(test_arming_one_destructive_row_does_not_arm_the_other);

    RUN_TEST(test_the_config_rows_sit_between_the_screens_and_the_destructive_ones);
    RUN_TEST(test_a_save_is_one_press);
    RUN_TEST(test_a_reload_is_one_press);

    RUN_TEST(test_the_selection_is_the_only_thing_this_layer_scrolls);
    RUN_TEST(test_reopening_the_list_goes_back_to_the_first_row);

    RUN_TEST(test_a_save_says_so_for_a_while);
    RUN_TEST(test_a_failure_is_its_own_word);
    RUN_TEST(test_the_result_belongs_to_one_row);
    RUN_TEST(test_a_second_outcome_replaces_the_first);
    RUN_TEST(test_the_saved_note_survives_the_millisecond_wrap);
    RUN_TEST(test_leaving_the_screen_drops_the_result);

    RUN_TEST(test_the_status_opens_on_the_first_page);
    RUN_TEST(test_the_pages_wrap);
    RUN_TEST(test_reopening_the_status_goes_back_to_the_first_page);
}
