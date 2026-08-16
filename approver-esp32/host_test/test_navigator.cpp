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
        case ScreenId::kWifi:
            nav.Navigate(Nav::kGear);
            nav.Navigate(Nav::kOpenWifi);
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
    const Nav swipes[] = {Nav::kSwipeLeft, Nav::kSwipeRight, Nav::kSwipeUp};

    for (Nav swipe : swipes) {
        Navigator settings;
        GoTo(settings, ScreenId::kSettings);
        TEST_ASSERT_FALSE(settings.Navigate(swipe));
        TEST_ASSERT_EQUAL_INT(static_cast<int>(ScreenId::kSettings),
                              static_cast<int>(settings.Screen()));

        Navigator wifi;
        GoTo(wifi, ScreenId::kWifi);
        TEST_ASSERT_FALSE(wifi.Navigate(swipe));
        TEST_ASSERT_EQUAL_INT(static_cast<int>(ScreenId::kWifi), static_cast<int>(wifi.Screen()));
    }
}

void test_settings_is_reached_from_the_clock_and_from_nowhere_else(void) {
    // §10.8.5 puts the gear on the clock, and `navigator.cpp` says why the
    // limits screen has no second way in: "one way in is one place to look
    // when it is not where it was expected". The rule is only a rule if the
    // other screen refuses.
    Navigator nav;
    GoTo(nav, ScreenId::kLimits);

    TEST_ASSERT_FALSE(nav.Navigate(Nav::kGear));
    TEST_ASSERT_FALSE(nav.Navigate(Nav::kSwipeUp));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ScreenId::kLimits), static_cast<int>(nav.Screen()));
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
    const ScreenId screens[] = {ScreenId::kClock, ScreenId::kLimits, ScreenId::kSettings,
                                ScreenId::kWifi};

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
    const Nav every[] = {Nav::kSwipeLeft, Nav::kSwipeRight, Nav::kSwipeUp,
                         Nav::kGear,      Nav::kBack,       Nav::kOpenWifi};

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

void RegisterNavigatorTests(void) {
    RUN_TEST(test_starts_on_the_clock_with_nothing_pending);

    RUN_TEST(test_clock_reaches_limits_by_either_swipe);
    RUN_TEST(test_limits_returns_to_the_clock);
    RUN_TEST(test_clock_reaches_settings_by_swipe_up_and_by_the_gear);
    RUN_TEST(test_wifi_is_reached_from_settings_and_backs_out_one_step);
    RUN_TEST(test_swipes_do_not_navigate_away_from_settings_or_wifi);
    RUN_TEST(test_settings_is_reached_from_the_clock_and_from_nowhere_else);
    RUN_TEST(test_wifi_is_only_opened_from_settings);
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
