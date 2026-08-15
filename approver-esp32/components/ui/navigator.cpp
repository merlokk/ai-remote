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
            // Back the way it came. **No swipe-up to settings from here**:
            // §10.8.5 puts the gear on the clock, and one way in is one place
            // to look when it is not where it was expected.
            if (nav == Nav::kSwipeLeft || nav == Nav::kSwipeRight || nav == Nav::kBack) {
                screen_ = ScreenId::kClock;
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
