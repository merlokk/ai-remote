#include "navigator.h"

namespace ui {

bool Navigator::Navigate(Nav nav) {
    // §10.8.1, and the first line of the function on purpose: while the card is
    // up there is no navigation. Not "navigation that is put back afterwards" —
    // none.
    if (RequestVisible()) {
        return false;
    }

    const ScreenId was = screen_;

    switch (screen_) {
        case ScreenId::kClock:
            // A two-position carousel: left and right both lead to the limits,
            // because there is nothing else out there to reach and a swipe that
            // does nothing in one direction reads as a broken screen.
            if (nav == Nav::kSwipeLeft || nav == Nav::kSwipeRight) {
                screen_ = ScreenId::kLimits;
            } else if (nav == Nav::kSwipeUp || nav == Nav::kGear) {
                screen_ = ScreenId::kSettings;
            }
            break;

        case ScreenId::kLimits:
            if (nav == Nav::kSwipeLeft || nav == Nav::kSwipeRight || nav == Nav::kBack) {
                screen_ = ScreenId::kClock;
            } else if (nav == Nav::kSwipeUp || nav == Nav::kGear) {
                // **This used to be refused, and the board is what changed it.**
                // The rule was "one way in is one place to look": §10.8.5 puts
                // the gear on the clock, so settings was reachable from the clock
                // and from nowhere else. That held while the limits screen was
                // something the operator swiped to — and §10.8.3 made it a screen
                // that **arrives**, every few seconds, for as long as a session is
                // spending. So a device left on the desk while Claude Code works
                // is a device parked on the one screen with no way into settings,
                // and the gesture that opens them does nothing.
                //
                // Found by trying to reach the settings list on a board that was
                // watching this very repository's status line. It is the same
                // action reaching the same place, so it is not a second way in;
                // what changed is that the first one stopped being reachable.
                screen_ = ScreenId::kSettings;
            }
            break;

        case ScreenId::kSettings:
            // **Swipes do not navigate here, and that is the whole rule for
            // this screen.** Settings is a scrolling list and §10.8.6 is a list
            // with a keyboard on it: a swipe belongs to the widget under the
            // finger. A navigation gesture that also scrolls is how a
            // half-typed password gets thrown away, which §10.8.1 spends a
            // paragraph on.
            if (nav == Nav::kBack) {
                screen_ = ScreenId::kClock;
            } else if (nav == Nav::kOpenWifi) {
                screen_ = ScreenId::kWifi;
            } else if (nav == Nav::kOpenStatus) {
                screen_ = ScreenId::kStatus;
            }
            break;

        case ScreenId::kStatus:
            // **A readout, and therefore swipeless for the reason settings is
            // not**: this one is paged, and the page is `BOOT` rather than a
            // gesture, so that a finger dragged across a wall of numbers moves
            // nothing at all. `kBack` goes up one level rather than home —
            // dropping the operator to the clock from two levels down is the
            // "forget this screen exists" the Wi-Fi row below already refuses.
            if (nav == Nav::kBack) {
                screen_ = ScreenId::kSettings;
            }
            break;

        case ScreenId::kWifi:
            if (nav == Nav::kBack) {
                screen_ = ScreenId::kSettings;
            }
            break;

        case ScreenId::kCount:
            break;
    }

    return screen_ != was;
}

bool Navigator::RequestArrived() {
    if (pending_ >= kMaxPending) {
        return false;
    }
    ++pending_;
    return true;
}

bool Navigator::LimitsArrived() {
    // §10.8.1: nothing outranks the card, and a readout least of all. The
    // document is still taken — `LimitsView` has it — this only declines to put
    // it on the glass.
    if (RequestVisible()) {
        return false;
    }
    // Only from the clock. Arriving numbers must not take an operator out of
    // settings or a half-typed password on the Wi-Fi screen, which is the rule
    // §10.8.1 spends a paragraph on and the reason this is not simply an
    // assignment.
    if (screen_ != ScreenId::kClock) {
        return false;
    }
    screen_ = ScreenId::kLimits;
    return true;
}

bool Navigator::LimitsWentQuiet() {
    if (screen_ != ScreenId::kLimits) {
        return false;
    }
    screen_ = ScreenId::kClock;
    return true;
}

bool Navigator::RequestAnswered() { return Drop(); }

bool Navigator::RequestExpired() { return Drop(); }

bool Navigator::Drop() {
    if (pending_ == 0) {
        return false;
    }
    --pending_;
    // `screen_` is deliberately untouched. That one omission is what §10.8.1
    // means by "what was underneath is restored exactly": there is nothing to
    // restore, because nothing was taken away.
    return true;
}

}  // namespace ui
