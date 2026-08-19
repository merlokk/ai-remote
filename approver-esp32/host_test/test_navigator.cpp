// The navigation state machine of CLAUDE.md §10.8.1, tested where it costs
// nothing to test it (§10.11, host tier).
//
// The three things §10.11 names for this machine are the three blocks below:
// a request preempts every screen, it cannot be dismissed, and what was
// underneath comes back with its state. The fourth block is the queue, whose
// only interesting case is being full — §10.14.1 asks for that to be a
// designed state rather than an error discovered later.

#include "navigator.h"
#include "unity.h"

using ui::Nav;
using ui::Navigator;
using ui::ScreenId;

namespace {

// Walks a fresh navigator to a screen, so a test can say where it starts
// instead of repeating the path that got it there.
void GoTo(Navigator &nav, ScreenId screen) {
    switch (screen) {
        case ScreenId::kClock:
            break;
        case ScreenId::kLimits:
            nav.Navigate(Nav::kSwipeLeft);
            break;
        case ScreenId::kSettings:
            nav.Navigate(Nav::kGear);
            break;
        case ScreenId::kStatus:
            nav.Navigate(Nav::kGear);
            nav.Navigate(Nav::kOpenStatus);
            break;
        case ScreenId::kTouch:
            nav.Navigate(Nav::kGear);
            nav.Navigate(Nav::kOpenTouch);
            break;
        case ScreenId::kWifi:
            nav.Navigate(Nav::kGear);
            nav.Navigate(Nav::kOpenWifi);
            break;
        case ScreenId::kWifiScan:
            nav.Navigate(Nav::kGear);
            nav.Navigate(Nav::kOpenWifi);
            nav.Navigate(Nav::kOpenScan);
            break;
        case ScreenId::kCount:
            break;
    }
    TEST_ASSERT_EQUAL_INT(static_cast<int>(screen), static_cast<int>(nav.Screen()));
}

// --- Where it starts -----------------------------------------------------

void test_starts_on_the_clock_with_nothing_pending(void) {
    Navigator nav;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ScreenId::kClock), static_cast<int>(nav.Screen()));
    TEST_ASSERT_FALSE(nav.RequestVisible());
    TEST_ASSERT_EQUAL_UINT8(0, nav.Pending());
    TEST_ASSERT_EQUAL_UINT8(0, nav.Waiting());
}

// --- Getting around (§10.8's table) --------------------------------------

void test_clock_reaches_limits_by_either_swipe(void) {
    Navigator left;
    TEST_ASSERT_TRUE(left.Navigate(Nav::kSwipeLeft));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ScreenId::kLimits), static_cast<int>(left.Screen()));

    Navigator right;
    TEST_ASSERT_TRUE(right.Navigate(Nav::kSwipeRight));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ScreenId::kLimits), static_cast<int>(right.Screen()));
}

void test_limits_returns_to_the_clock(void) {
    Navigator nav;
    GoTo(nav, ScreenId::kLimits);
    TEST_ASSERT_TRUE(nav.Navigate(Nav::kSwipeRight));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ScreenId::kClock), static_cast<int>(nav.Screen()));
}

void test_clock_reaches_settings_by_swipe_up_and_by_the_gear(void) {
    Navigator swipe;
    TEST_ASSERT_TRUE(swipe.Navigate(Nav::kSwipeUp));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ScreenId::kSettings), static_cast<int>(swipe.Screen()));

    Navigator gear;
    TEST_ASSERT_TRUE(gear.Navigate(Nav::kGear));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ScreenId::kSettings), static_cast<int>(gear.Screen()));
}

void test_wifi_is_reached_from_settings_and_backs_out_one_step(void) {
    Navigator nav;
    GoTo(nav, ScreenId::kWifi);

    // Back from Wi-Fi is Settings, not the clock: §10.8.6 is a screen *inside*
    // settings, and dropping the operator two levels is how a "forget this
    // network" turns into hunting for where they were.
    TEST_ASSERT_TRUE(nav.Navigate(Nav::kBack));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ScreenId::kSettings), static_cast<int>(nav.Screen()));
    TEST_ASSERT_TRUE(nav.Navigate(Nav::kBack));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ScreenId::kClock), static_cast<int>(nav.Screen()));
}

void test_swipes_do_not_navigate_away_from_settings_or_wifi(void) {
    // The rule that protects the half-typed password of §10.8.1: on a list and
    // on a keyboard, a swipe belongs to the widget under the finger.
    //
    // **Half of it changed when the settings list started scrolling** (§10.8.5),
    // and this is where the difference is asserted rather than described. On that
    // screen a *vertical* swipe is the list — and if one ever reaches the navigator
    // anyway it must still move nothing, which is what the loop below says — while
    // sideways is now the way out, because a list a thumb can drag and cannot leave
    // is a list people get stuck in. The Wi-Fi screen is unchanged: nothing there
    // scrolls, and a swipe on a record being edited belongs to the record.
    const Nav swipes[] = {Nav::kSwipeLeft, Nav::kSwipeRight, Nav::kSwipeUp};

    for (Nav swipe : swipes) {
        Navigator settings;
        GoTo(settings, ScreenId::kSettings);
        const bool leaves = swipe == Nav::kSwipeLeft || swipe == Nav::kSwipeRight;
        TEST_ASSERT_EQUAL(leaves, settings.Navigate(swipe));
        TEST_ASSERT_EQUAL_INT(
            static_cast<int>(leaves ? ScreenId::kClock : ScreenId::kSettings),
            static_cast<int>(settings.Screen()));

        Navigator wifi;
        GoTo(wifi, ScreenId::kWifi);
        TEST_ASSERT_FALSE(wifi.Navigate(swipe));
        TEST_ASSERT_EQUAL_INT(static_cast<int>(ScreenId::kWifi), static_cast<int>(wifi.Screen()));
    }
}

void test_settings_is_reached_from_the_limits_screen_too(void) {
    // Because that screen **arrives** rather than being swiped to (§10.8.3): a
    // device watching a working session sits on it, and settings has to be
    // reachable from where the operator actually is.
    Navigator swipe;
    GoTo(swipe, ScreenId::kLimits);
    TEST_ASSERT_TRUE(swipe.Navigate(Nav::kSwipeUp));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ScreenId::kSettings), static_cast<int>(swipe.Screen()));

    Navigator gear;
    GoTo(gear, ScreenId::kLimits);
    TEST_ASSERT_TRUE(gear.Navigate(Nav::kGear));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ScreenId::kSettings), static_cast<int>(gear.Screen()));

    // And back out of settings is home rather than to the screen it came from:
    // the limits are a place the device put the operator, not one they chose.
    TEST_ASSERT_TRUE(gear.Navigate(Nav::kBack));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ScreenId::kClock), static_cast<int>(gear.Screen()));
}

void test_settings_is_reached_from_the_two_screens_that_are_home(void) {
    // The clock and the limits, and **nowhere deeper**. Settings is one level
    // down from home, so a screen that is already inside it has no way to open
    // it again — the way out of those is `kBack`, and a gesture that took the
    // operator sideways into a list they are already inside is a gesture that
    // loses their place.
    const ScreenId inside[] = {ScreenId::kStatus, ScreenId::kTouch, ScreenId::kWifi,
                               ScreenId::kWifiScan};
    for (ScreenId screen : inside) {
        Navigator nav;
        GoTo(nav, screen);
        TEST_ASSERT_FALSE(nav.Navigate(Nav::kGear));
        TEST_ASSERT_FALSE(nav.Navigate(Nav::kSwipeUp));
        TEST_ASSERT_EQUAL_INT(static_cast<int>(screen), static_cast<int>(nav.Screen()));
    }
}

void test_wifi_is_only_opened_from_settings(void) {
    // The Wi-Fi screen is *inside* settings (§10.8.6), so the action that
    // opens it has to be inert everywhere else — otherwise a stray event from
    // the wrong screen lands the operator on a keyboard they did not ask for.
    Navigator clock;
    TEST_ASSERT_FALSE(clock.Navigate(Nav::kOpenWifi));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ScreenId::kClock), static_cast<int>(clock.Screen()));

    Navigator limits;
    GoTo(limits, ScreenId::kLimits);
    TEST_ASSERT_FALSE(limits.Navigate(Nav::kOpenWifi));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ScreenId::kLimits), static_cast<int>(limits.Screen()));

    // And from the Wi-Fi screen itself it is not a way to reload it.
    Navigator wifi;
    GoTo(wifi, ScreenId::kWifi);
    TEST_ASSERT_FALSE(wifi.Navigate(Nav::kOpenWifi));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ScreenId::kWifi), static_cast<int>(wifi.Screen()));
}

void test_back_on_the_clock_does_nothing(void) {
    Navigator nav;
    TEST_ASSERT_FALSE(nav.Navigate(Nav::kBack));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ScreenId::kClock), static_cast<int>(nav.Screen()));
}

// --- The card outranks everything (§10.8.1) ------------------------------

void test_a_request_appears_over_every_screen_without_moving_it(void) {
    const ScreenId screens[] = {ScreenId::kClock,  ScreenId::kLimits, ScreenId::kSettings,
                                ScreenId::kStatus, ScreenId::kTouch,  ScreenId::kWifi,
                                ScreenId::kWifiScan};

    for (ScreenId screen : screens) {
        Navigator nav;
        GoTo(nav, screen);

        TEST_ASSERT_TRUE(nav.RequestArrived());
        TEST_ASSERT_TRUE(nav.RequestVisible());
        // The card is over it, not instead of it.
        TEST_ASSERT_EQUAL_INT(static_cast<int>(screen), static_cast<int>(nav.Screen()));
    }
}

void test_navigation_is_gone_while_the_card_is_up(void) {
    const Nav every[] = {Nav::kSwipeLeft, Nav::kSwipeRight, Nav::kSwipeUp,    Nav::kGear,
                         Nav::kBack,      Nav::kOpenWifi,   Nav::kOpenStatus, Nav::kOpenTouch,
                         Nav::kOpenScan};

    Navigator nav;
    GoTo(nav, ScreenId::kSettings);
    TEST_ASSERT_TRUE(nav.RequestArrived());

    for (Nav action : every) {
        TEST_ASSERT_FALSE(nav.Navigate(action));
        TEST_ASSERT_EQUAL_INT(static_cast<int>(ScreenId::kSettings), static_cast<int>(nav.Screen()));
        // And it stays up: no navigation gesture is a way out of the card.
        TEST_ASSERT_TRUE(nav.RequestVisible());
    }
}

void test_answering_restores_the_screen_underneath_and_navigation_with_it(void) {
    Navigator nav;
    GoTo(nav, ScreenId::kWifi);

    nav.RequestArrived();
    TEST_ASSERT_TRUE(nav.RequestAnswered());

    TEST_ASSERT_FALSE(nav.RequestVisible());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ScreenId::kWifi), static_cast<int>(nav.Screen()));
    TEST_ASSERT_TRUE(nav.Navigate(Nav::kBack));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ScreenId::kSettings), static_cast<int>(nav.Screen()));
}

void test_expiring_leaves_the_same_state_as_answering(void) {
    // §10.10: an expired card is silence, not a deny. That difference is real
    // and it is not this class's — the screen does the same thing either way,
    // and this test is what pins that they have not drifted apart.
    Navigator answered;
    Navigator expired;
    GoTo(answered, ScreenId::kLimits);
    GoTo(expired, ScreenId::kLimits);

    answered.RequestArrived();
    expired.RequestArrived();
    TEST_ASSERT_TRUE(answered.RequestAnswered());
    TEST_ASSERT_TRUE(expired.RequestExpired());

    TEST_ASSERT_EQUAL_INT(static_cast<int>(answered.Screen()), static_cast<int>(expired.Screen()));
    TEST_ASSERT_EQUAL_UINT8(answered.Pending(), expired.Pending());
    TEST_ASSERT_FALSE(answered.RequestVisible());
    TEST_ASSERT_FALSE(expired.RequestVisible());
}

// --- More than one waiting (§10.8.4) -------------------------------------

void test_a_second_request_waits_behind_the_first(void) {
    Navigator nav;
    TEST_ASSERT_TRUE(nav.RequestArrived());
    TEST_ASSERT_EQUAL_UINT8(0, nav.Waiting());

    TEST_ASSERT_TRUE(nav.RequestArrived());
    TEST_ASSERT_TRUE(nav.RequestArrived());
    // Three pending is one on the card and "+2 waiting" beside it.
    TEST_ASSERT_EQUAL_UINT8(3, nav.Pending());
    TEST_ASSERT_EQUAL_UINT8(2, nav.Waiting());
}

void test_answering_one_brings_the_next_up_immediately(void) {
    Navigator nav;
    nav.RequestArrived();
    nav.RequestArrived();

    TEST_ASSERT_TRUE(nav.RequestAnswered());
    // Still up, because there is another one — §10.8.4, and the moment the
    // 300 ms guard of §10.8.1 earns its place.
    TEST_ASSERT_TRUE(nav.RequestVisible());
    TEST_ASSERT_EQUAL_UINT8(0, nav.Waiting());

    TEST_ASSERT_TRUE(nav.RequestAnswered());
    TEST_ASSERT_FALSE(nav.RequestVisible());
}

void test_the_queue_is_bounded_and_says_so(void) {
    Navigator nav;
    for (uint8_t i = 0; i < Navigator::kMaxPending; ++i) {
        TEST_ASSERT_TRUE(nav.RequestArrived());
    }

    // The overflow is refused rather than absorbed: §10.14.1 wants "full" to
    // be a state the caller handles, and §10.10 already says what handling it
    // means — drop it, one log line, no reply.
    TEST_ASSERT_FALSE(nav.RequestArrived());
    TEST_ASSERT_EQUAL_UINT8(Navigator::kMaxPending, nav.Pending());

    // And it recovers: room freed is room usable.
    TEST_ASSERT_TRUE(nav.RequestAnswered());
    TEST_ASSERT_TRUE(nav.RequestArrived());
    TEST_ASSERT_EQUAL_UINT8(Navigator::kMaxPending, nav.Pending());
}

void test_answering_nothing_is_refused_rather_than_wrapping(void) {
    // `pending_` is unsigned, so the failure this guards against is not "goes
    // to -1" but "goes to 255 and the card is up forever".
    Navigator nav;
    TEST_ASSERT_FALSE(nav.RequestAnswered());
    TEST_ASSERT_FALSE(nav.RequestExpired());
    TEST_ASSERT_EQUAL_UINT8(0, nav.Pending());
    TEST_ASSERT_FALSE(nav.RequestVisible());
}

}  // namespace

// --- The status pages, inside settings (§10.8.5) -------------------------

void test_status_is_reached_from_settings_and_backs_out_one_step(void) {
    // One step, like the Wi-Fi screen next to it: dropping the operator to the
    // clock from two levels down is the "where did that screen go" this rule
    // exists to avoid.
    Navigator nav;
    GoTo(nav, ScreenId::kStatus);
    TEST_ASSERT_TRUE(nav.Navigate(Nav::kBack));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ScreenId::kSettings), static_cast<int>(nav.Screen()));
    TEST_ASSERT_TRUE(nav.Navigate(Nav::kBack));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ScreenId::kClock), static_cast<int>(nav.Screen()));
}

void test_status_is_only_opened_from_settings(void) {
    const ScreenId elsewhere[] = {ScreenId::kClock, ScreenId::kLimits, ScreenId::kWifi,
                                  ScreenId::kWifiScan};
    for (ScreenId screen : elsewhere) {
        Navigator nav;
        GoTo(nav, screen);
        TEST_ASSERT_FALSE(nav.Navigate(Nav::kOpenStatus));
        TEST_ASSERT_EQUAL_INT(static_cast<int>(screen), static_cast<int>(nav.Screen()));
    }
}

void test_swipes_do_not_navigate_away_from_the_status(void) {
    // A wall of numbers with a finger dragged across it must move nothing:
    // the page is `BOOT`, deliberately, so that reading is not navigating.
    const Nav swipes[] = {Nav::kSwipeLeft, Nav::kSwipeRight, Nav::kSwipeUp};
    for (Nav swipe : swipes) {
        Navigator nav;
        GoTo(nav, ScreenId::kStatus);
        TEST_ASSERT_FALSE(nav.Navigate(swipe));
        TEST_ASSERT_EQUAL_INT(static_cast<int>(ScreenId::kStatus), static_cast<int>(nav.Screen()));
    }
}

void test_arriving_limits_do_not_take_the_operator_out_of_the_status(void) {
    // §10.8.1's "everything else is quiet", and the status screen is the newest
    // place it could have been broken.
    Navigator nav;
    GoTo(nav, ScreenId::kStatus);
    TEST_ASSERT_FALSE(nav.LimitsArrived());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ScreenId::kStatus), static_cast<int>(nav.Screen()));
}

void test_the_touch_test_is_reached_from_settings_and_is_swipeless(void) {
    // **Swipeless is a safety property here, not a preference**: this is the
    // screen that tests the thing a swipe is made of, so a gesture that
    // navigated would take a device with a bad correction off the one screen
    // that can fix it — by accident, while a finger is being dragged across it.
    Navigator nav;
    GoTo(nav, ScreenId::kTouch);

    const Nav swipes[] = {Nav::kSwipeLeft, Nav::kSwipeRight, Nav::kSwipeUp};
    for (Nav swipe : swipes) {
        TEST_ASSERT_FALSE(nav.Navigate(swipe));
        TEST_ASSERT_EQUAL_INT(static_cast<int>(ScreenId::kTouch), static_cast<int>(nav.Screen()));
    }

    TEST_ASSERT_TRUE(nav.Navigate(Nav::kBack));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ScreenId::kSettings), static_cast<int>(nav.Screen()));
}

void test_the_touch_test_is_only_opened_from_settings(void) {
    const ScreenId elsewhere[] = {ScreenId::kClock, ScreenId::kLimits, ScreenId::kStatus};
    for (ScreenId screen : elsewhere) {
        Navigator nav;
        GoTo(nav, screen);
        TEST_ASSERT_FALSE(nav.Navigate(Nav::kOpenTouch));
        TEST_ASSERT_EQUAL_INT(static_cast<int>(screen), static_cast<int>(nav.Screen()));
    }
}

// --- The scan list, inside the Wi-Fi screen (§10.8.6) --------------------

void test_the_scan_list_is_reached_from_the_wifi_screen_and_backs_out_one_step(void) {
    // Two levels down from home, and each `kBack` is one of them: out of the
    // list is the record it was opened for — which is where the name it just
    // picked has landed — and out of that is the settings list.
    Navigator nav;
    GoTo(nav, ScreenId::kWifi);
    TEST_ASSERT_TRUE(nav.Navigate(Nav::kOpenScan));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ScreenId::kWifiScan), static_cast<int>(nav.Screen()));

    TEST_ASSERT_TRUE(nav.Navigate(Nav::kBack));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ScreenId::kWifi), static_cast<int>(nav.Screen()));
    TEST_ASSERT_TRUE(nav.Navigate(Nav::kBack));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ScreenId::kSettings), static_cast<int>(nav.Screen()));
}

void test_the_scan_list_is_only_opened_from_the_wifi_screen(void) {
    // A scan costs a connected station a beat because the radio has to leave its
    // channel (§10.8.6), so the action that starts one has to be inert
    // everywhere the operator did not ask for it — including on the list itself,
    // where it would otherwise be a way to reload it and lose the selection.
    const ScreenId elsewhere[] = {ScreenId::kClock, ScreenId::kLimits, ScreenId::kSettings,
                                  ScreenId::kStatus, ScreenId::kTouch, ScreenId::kWifiScan};
    for (ScreenId screen : elsewhere) {
        Navigator nav;
        GoTo(nav, screen);
        TEST_ASSERT_FALSE(nav.Navigate(Nav::kOpenScan));
        TEST_ASSERT_EQUAL_INT(static_cast<int>(screen), static_cast<int>(nav.Screen()));
    }
}

void test_swipes_do_not_navigate_away_from_the_scan_list(void) {
    // The rows are a finger apart and one of them is under the finger: a
    // gesture that navigated would take the operator off the list while they
    // were aiming at a network.
    const Nav swipes[] = {Nav::kSwipeLeft, Nav::kSwipeRight, Nav::kSwipeUp, Nav::kGear};
    for (Nav swipe : swipes) {
        Navigator nav;
        GoTo(nav, ScreenId::kWifiScan);
        TEST_ASSERT_FALSE(nav.Navigate(swipe));
        TEST_ASSERT_EQUAL_INT(static_cast<int>(ScreenId::kWifiScan),
                              static_cast<int>(nav.Screen()));
    }
}

void test_arriving_limits_do_not_take_the_operator_out_of_the_scan_list(void) {
    // §10.8.1's "everything else is quiet", on the deepest screen there is —
    // and the one where being thrown out would lose a selection rather than
    // just a place.
    Navigator nav;
    GoTo(nav, ScreenId::kWifiScan);
    TEST_ASSERT_FALSE(nav.LimitsArrived());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ScreenId::kWifiScan), static_cast<int>(nav.Screen()));
}

// --- The settings list scrolls, so its way out moved (§10.8.5) -----------

void test_a_sideways_swipe_leaves_the_settings_list(void) {
    // **The rule the scrolling bought.** A vertical drag on that screen is the
    // list — LVGL suppresses the gesture while it scrolls, so a swipe down never
    // reaches the navigator at all — and a list somebody can drag with a thumb and
    // cannot leave with one is a list people get stuck in. Both directions, for the
    // reason the clock's carousel gives: a swipe that works one way and not the
    // other reads as a broken screen.
    const ui::Nav sideways_both[] = {ui::Nav::kSwipeLeft, ui::Nav::kSwipeRight};
    for (ui::Nav sideways : sideways_both) {
        ui::Navigator nav;
        nav.Navigate(ui::Nav::kSwipeUp);
        TEST_ASSERT_EQUAL_INT(static_cast<int>(ui::ScreenId::kSettings),
                              static_cast<int>(nav.Screen()));
        nav.Navigate(sideways);
        TEST_ASSERT_EQUAL_INT(static_cast<int>(ui::ScreenId::kClock),
                              static_cast<int>(nav.Screen()));
    }
}

void test_a_sideways_swipe_does_not_leave_the_screens_behind_settings(void) {
    // It is the *list* that lost its swipe down, because the list is what scrolls.
    // The status pages, the touch test and the Wi-Fi screens refuse every swipe on
    // their own grounds (a wall of numbers that must not move under a finger, a
    // calibration that must not be swiped away from, a record being edited), and
    // none of that changed.
    const ui::ScreenId deeper[] = {ui::ScreenId::kStatus, ui::ScreenId::kTouch,
                                   ui::ScreenId::kWifi};
    const ui::Nav opens[] = {ui::Nav::kOpenStatus, ui::Nav::kOpenTouch, ui::Nav::kOpenWifi};
    for (int i = 0; i < 3; ++i) {
        const ui::Nav sideways_both[] = {ui::Nav::kSwipeLeft, ui::Nav::kSwipeRight};
    for (ui::Nav sideways : sideways_both) {
            ui::Navigator nav;
            nav.Navigate(ui::Nav::kSwipeUp);
            nav.Navigate(opens[i]);
            TEST_ASSERT_EQUAL_INT(static_cast<int>(deeper[i]), static_cast<int>(nav.Screen()));
            nav.Navigate(sideways);
            TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(deeper[i]),
                                          static_cast<int>(nav.Screen()),
                                          "a swipe moved a screen that refuses swipes");
        }
    }
}

void RegisterNavigatorTests(void) {
    RUN_TEST(test_a_sideways_swipe_leaves_the_settings_list);
    RUN_TEST(test_a_sideways_swipe_does_not_leave_the_screens_behind_settings);
    RUN_TEST(test_starts_on_the_clock_with_nothing_pending);

    RUN_TEST(test_clock_reaches_limits_by_either_swipe);
    RUN_TEST(test_limits_returns_to_the_clock);
    RUN_TEST(test_clock_reaches_settings_by_swipe_up_and_by_the_gear);
    RUN_TEST(test_wifi_is_reached_from_settings_and_backs_out_one_step);
    RUN_TEST(test_swipes_do_not_navigate_away_from_settings_or_wifi);
    RUN_TEST(test_settings_is_reached_from_the_two_screens_that_are_home);
    RUN_TEST(test_settings_is_reached_from_the_limits_screen_too);
    RUN_TEST(test_wifi_is_only_opened_from_settings);

    RUN_TEST(test_status_is_reached_from_settings_and_backs_out_one_step);
    RUN_TEST(test_status_is_only_opened_from_settings);
    RUN_TEST(test_the_touch_test_is_reached_from_settings_and_is_swipeless);
    RUN_TEST(test_the_touch_test_is_only_opened_from_settings);
    RUN_TEST(test_swipes_do_not_navigate_away_from_the_status);
    RUN_TEST(test_the_scan_list_is_reached_from_the_wifi_screen_and_backs_out_one_step);
    RUN_TEST(test_the_scan_list_is_only_opened_from_the_wifi_screen);
    RUN_TEST(test_swipes_do_not_navigate_away_from_the_scan_list);
    RUN_TEST(test_arriving_limits_do_not_take_the_operator_out_of_the_scan_list);
    RUN_TEST(test_arriving_limits_do_not_take_the_operator_out_of_the_status);
    RUN_TEST(test_back_on_the_clock_does_nothing);

    RUN_TEST(test_a_request_appears_over_every_screen_without_moving_it);
    RUN_TEST(test_navigation_is_gone_while_the_card_is_up);
    RUN_TEST(test_answering_restores_the_screen_underneath_and_navigation_with_it);
    RUN_TEST(test_expiring_leaves_the_same_state_as_answering);

    RUN_TEST(test_a_second_request_waits_behind_the_first);
    RUN_TEST(test_answering_one_brings_the_next_up_immediately);
    RUN_TEST(test_the_queue_is_bounded_and_says_so);
    RUN_TEST(test_answering_nothing_is_refused_rather_than_wrapping);
}
