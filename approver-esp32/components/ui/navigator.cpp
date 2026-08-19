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
            // **A vertical swipe belongs to the list, and a sideways one is the way
            // out.** This screen used to refuse every swipe on the reasoning that a
            // gesture which also scrolls is how a half-typed password gets thrown
            // away (§10.8.1) — and then the list really did start scrolling, at the
            // repository owner's request, which settles the first half and takes
            // away the second: LVGL suppresses a gesture while it is scrolling, so
            // a drag up or down never reaches here at all.
            //
            // What that cost is the swipe *down* that used to leave, and something
            // had to replace it: `PWR` and the title are both still there, but a
            // list you can drag with a thumb and cannot leave with one is a list
            // people get stuck in. So sideways goes back — **both** directions, for
            // the reason the clock's own carousel gives: a swipe that does nothing
            // in one direction reads as a broken screen.
            if (nav == Nav::kBack || nav == Nav::kSwipeLeft || nav == Nav::kSwipeRight) {
                screen_ = ScreenId::kClock;
            } else if (nav == Nav::kOpenWifi) {
                screen_ = ScreenId::kWifi;
            } else if (nav == Nav::kOpenStatus) {
                screen_ = ScreenId::kStatus;
            } else if (nav == Nav::kOpenTouch) {
                screen_ = ScreenId::kTouch;
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

        case ScreenId::kTouch:
            // **Swipeless, and here that is a safety property rather than a
            // preference.** This is the screen that tests the thing a swipe is
            // made of: a gesture that navigated would mean a device with a bad
            // correction leaves the one screen that can fix it, by accident,
            // while the operator is dragging a finger across it to see where the
            // points land. `PWR` is the way out, and it is a button.
            if (nav == Nav::kBack) {
                screen_ = ScreenId::kSettings;
            }
            break;

        case ScreenId::kWifi:
            // **Swipeless, like the settings list it hangs off**: this screen is
            // rows and arrows drawn a finger's width apart, and a gesture that
            // also navigated would take the operator off it while they were
            // aiming at one of them.
            if (nav == Nav::kBack) {
                screen_ = ScreenId::kSettings;
            } else if (nav == Nav::kOpenScan) {
                screen_ = ScreenId::kWifiScan;
            }
            break;

        case ScreenId::kWifiScan:
            // **Back is one level, to the record the list was opened for.** The
            // whole point of picking a name off the air is that it lands in the
            // record that was on the glass, so dropping the operator two levels
            // out would hide the one thing that changed.
            if (nav == Nav::kBack) {
                screen_ = ScreenId::kWifi;
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
