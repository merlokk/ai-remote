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
using ui::StatusPage;
using ui::StatusPager;

namespace {

constexpr uint8_t kWifi = static_cast<uint8_t>(SettingsEntry::kWifi);
constexpr uint8_t kStatus = static_cast<uint8_t>(SettingsEntry::kStatus);
constexpr uint8_t kTouch = static_cast<uint8_t>(SettingsEntry::kTouch);
constexpr uint8_t kReboot = static_cast<uint8_t>(SettingsEntry::kReboot);

}  // namespace

// --- The list ------------------------------------------------------------

void test_the_menu_opens_at_the_top(void) {
    SettingsMenu menu;
    TEST_ASSERT_EQUAL_UINT8(0, menu.Selected());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(SettingsEntry::kWifi),
                          static_cast<int>(menu.SelectedEntry()));
}

void test_reboot_is_the_last_row(void) {
    // The owner's order, and the one part of it that is a safety property rather
    // than a preference: the row that restarts the device is as far as possible
    // from the row a finger lands on when the screen opens.
    TEST_ASSERT_EQUAL_UINT8(SettingsMenu::kEntryCount - 1, kReboot);
    TEST_ASSERT_NOT_EQUAL(kReboot, 0);
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

void test_next_does_not_skip_the_rows_with_nothing_behind_them(void) {
    // A selection that jumps over what it is pointing at is a selection nobody
    // can predict. The row is what explains why it does nothing, and that is a
    // better place for the explanation than a gap in the walk.
    SettingsMenu menu;
    menu.Next();
    TEST_ASSERT_EQUAL_UINT8(kStatus, menu.Selected());
    menu.Next();
    TEST_ASSERT_EQUAL_UINT8(kTouch, menu.Selected());
    TEST_ASSERT_FALSE(SettingsMenu::Built(SettingsEntry::kTouch));
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

void test_a_row_with_nothing_behind_it_says_so_rather_than_navigating(void) {
    // §10.9's "unknown is the honest state", applied to a menu: the row is on the
    // list because the device is going to have it, and pressing it must not look
    // like a screen that failed to open.
    const uint8_t unbuilt[] = {kWifi, kTouch};
    for (uint8_t row : unbuilt) {
        SettingsMenu menu;
        menu.Select(row);
        TEST_ASSERT_EQUAL_INT(static_cast<int>(SettingsAction::kNotBuilt),
                              static_cast<int>(menu.Activate(1000)));
    }
}

// --- Reboot, which is the whole reason this file is worth having ---------

void test_one_press_on_reboot_does_not_reboot(void) {
    SettingsMenu menu;
    menu.Select(kReboot);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(SettingsAction::kArmed),
                          static_cast<int>(menu.Activate(1000)));
    TEST_ASSERT_TRUE(menu.RebootArmed(1000));
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

    TEST_ASSERT_FALSE(menu.RebootArmed(1000 + SettingsMenu::kArmedMs));
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
    TEST_ASSERT_FALSE(armed_by_next.RebootArmed(1100));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(SettingsAction::kArmed),
                          static_cast<int>(armed_by_next.Activate(1100)));

    SettingsMenu armed_by_tap;
    armed_by_tap.Select(kReboot);
    armed_by_tap.Activate(1000);
    armed_by_tap.Select(kStatus);
    armed_by_tap.Select(kReboot);
    TEST_ASSERT_FALSE(armed_by_tap.RebootArmed(1100));
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
    TEST_ASSERT_FALSE(menu.RebootArmed(1100));
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
    TEST_ASSERT_TRUE(menu.RebootArmed(1100));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(SettingsAction::kReboot),
                          static_cast<int>(menu.Activate(1100)));
}

void test_leaving_and_coming_back_disarms_and_goes_to_the_top(void) {
    SettingsMenu menu;
    menu.Select(kReboot);
    menu.Activate(1000);
    menu.Opened();
    TEST_ASSERT_EQUAL_UINT8(0, menu.Selected());
    TEST_ASSERT_FALSE(menu.RebootArmed(1100));
}

void test_the_arming_window_survives_the_millisecond_wrap(void) {
    // Both sides of it: a window written `now < armed_at + window` passes every
    // check taken after the counter wraps and fails only in the moments just
    // before, which is where an unexpected reboot would come from.
    SettingsMenu menu;
    menu.Select(kReboot);
    menu.Activate(0xFFFFFF00u);

    TEST_ASSERT_TRUE(menu.RebootArmed(0xFFFFFF10u));  // 16 ms later, not yet wrapped
    TEST_ASSERT_TRUE(menu.RebootArmed(400));          // wrapped, 656 ms after
    TEST_ASSERT_FALSE(menu.RebootArmed(0xFFFFFF00u + SettingsMenu::kArmedMs));
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
    RUN_TEST(test_reboot_is_the_last_row);
    RUN_TEST(test_next_walks_every_row_and_wraps);
    RUN_TEST(test_next_does_not_skip_the_rows_with_nothing_behind_them);
    RUN_TEST(test_a_tap_between_rows_selects_nothing);

    RUN_TEST(test_the_status_row_opens_the_status);
    RUN_TEST(test_a_row_with_nothing_behind_it_says_so_rather_than_navigating);

    RUN_TEST(test_one_press_on_reboot_does_not_reboot);
    RUN_TEST(test_a_second_press_inside_the_window_reboots);
    RUN_TEST(test_the_arming_expires_and_the_next_press_arms_again);
    RUN_TEST(test_moving_off_the_reboot_row_disarms_it);
    RUN_TEST(test_walking_the_list_round_with_the_button_disarms_the_reboot);
    RUN_TEST(test_selecting_the_reboot_row_again_does_not_disarm_it);
    RUN_TEST(test_leaving_and_coming_back_disarms_and_goes_to_the_top);
    RUN_TEST(test_the_arming_window_survives_the_millisecond_wrap);

    RUN_TEST(test_the_status_opens_on_the_first_page);
    RUN_TEST(test_the_pages_wrap);
    RUN_TEST(test_reopening_the_status_goes_back_to_the_first_page);
}
