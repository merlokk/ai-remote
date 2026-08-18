#pragma once

// Which screen is up, and why (CLAUDE.md §10.8.1).
//
// **This file includes nothing.** Not LVGL, not ESP-IDF, not a single header —
// which is the point rather than an accident. §10.11 makes the host tier the
// comprehensive one, and a navigation state machine that reaches for a widget
// is a machine that can only be tested on a board. So the decision — *what*
// should be on the screen — lives here, and applying it to LVGL is somebody
// else's twenty lines.
//
// It is logic rather than library (§10.14.2), and it is the first thing in
// `components/` that is: it lives there because that is what makes it
// compilable by the host tests, not because it knows about wires.
//
// The rules it exists to enforce, all of them §10.8.1's:
//
//   * **the request card outranks everything.** It appears over whatever is
//     up, and while it is up there is no navigation at all — no swipe, no
//     gear, no back. A stray swipe that hides a pending request is a request
//     that times out silently;
//   * **it cannot be dismissed.** There is no `Dismiss()` here, and the
//     absence is the design: a third way out of the card would be a third
//     verdict, and §7 has two;
//   * **what was underneath comes back exactly.** Structurally, not by
//     restoring anything — the screen simply never changed while the card was
//     up, so the settings list keeps its scroll position and the Wi-Fi screen
//     keeps its half-typed password.
//
// What it deliberately does **not** hold is the requests themselves. A count
// is all the screen needs ("+2 waiting"), and a navigator that stored
// `tool_name` would be a navigator with the protocol in it.

#include <cstdint>

namespace ui {

// The five screens of §10.8, minus the one that is not a screen: the request
// card is an overlay with its own flag below, because it is reached by a
// message rather than by the operator and cannot be navigated to or away from.
enum class ScreenId : uint8_t {
    kClock = 0,
    kLimits,
    kSettings,
    kStatus,
    kWifi,
    kCount,
};

// What the operator can do. Not "events" in general — nothing the bus does
// appears here, and nothing here is a gesture the IMU could produce (§10.13:
// no gesture ever approves anything, and the way to keep that true is for the
// gestures that exist to be unable to reach a verdict at all).
enum class Nav : uint8_t {
    kSwipeLeft,
    kSwipeRight,
    kSwipeUp,
    kGear,
    kBack,
    kOpenWifi,
    kOpenStatus,
};

class Navigator {
   public:
    // **Full is a state that had to be designed** (§10.14.1). Four is more
    // than the operator will ever see waiting, and the fifth arrival is
    // dropped with no reply — which is already the fail-safe (§10.10), not a
    // new behaviour invented for a full queue.
    static constexpr uint8_t kMaxPending = 4;

    Navigator() = default;
    Navigator(const Navigator &) = delete;
    Navigator &operator=(const Navigator &) = delete;

    ScreenId Screen() const { return screen_; }

    // The card is up exactly while something is pending. There is no separate
    // "visible" flag to get out of step with the count.
    bool RequestVisible() const { return pending_ > 0; }

    uint8_t Pending() const { return pending_; }

    // What the card's "+N waiting" shows: everything except the one on screen.
    uint8_t Waiting() const {
        return pending_ > 0 ? static_cast<uint8_t>(pending_ - 1) : uint8_t{0};
    }

    // Returns true if the screen changed. **Ignored entirely while the card is
    // up** — not queued, not deferred: a swipe made while the operator was
    // looking at a permission request was aimed at something that is no longer
    // there.
    bool Navigate(Nav nav);

    // False when the queue is full, and the caller's job is then §10.10's:
    // drop it, one log line, no reply.
    bool RequestArrived();

    // --- the limits screen (§10.8.3) --------------------------------------
    //
    // **It arrives rather than being navigated to**, which is the one place this
    // firmware departs from §10.8.3 — see `limits_view.h` for why and at whose
    // request. The swipe below still reaches it; nothing depends on that.
    //
    // Raising it is subject to the same rule everything else is: **the request
    // card outranks it** (§10.8.1). A `status` document that lands while a
    // permission request is on the glass changes what the limits screen *shows*
    // and never what is up — which is this section's "everything else is quiet",
    // applied to the one screen that could otherwise steal focus for a readout.
    //
    // Returns true if the screen changed.
    bool LimitsArrived();

    // A minute with nothing (`ui::LimitsView::kQuietMs`). Back to the clock, and
    // only from the limits screen — a stream going quiet while the operator is in
    // settings must not throw them out of it.
    bool LimitsWentQuiet();

    // Answered — a decision was signed and published — and expired — the
    // countdown ran out and nothing was sent. **The navigator cannot tell them
    // apart and should not:** the difference is about what left the device,
    // and this class only decides what is on the glass. Two names because the
    // call sites mean different things, one behaviour because the screen does
    // not.
    //
    // Both return false if nothing was pending, which is a bug upstream rather
    // than a state to reach.
    bool RequestAnswered();
    bool RequestExpired();

   private:
    bool Drop();

    ScreenId screen_ = ScreenId::kClock;
    uint8_t pending_ = 0;
};

}  // namespace ui
