#pragma once

// The limits screen (CLAUDE.md §10.8.3), and the part of it that decides.
//
// **This file includes `<cstdint>`, `<cstddef>` and the navigator**, so all of it
// runs under Unity with no board (§10.11) — the tenth subject in this firmware to
// manage that. The pixels are `screens/limits_screen.cpp`'s.
//
// ## It arrives; it is not navigated to
//
// §10.8.3 specifies this screen as one you swipe to from the clock. **It is
// arrival-driven instead**, at the repository owner's request, and the change is
// worth stating because it makes the screen a different kind of thing:
//
//   * a `status` document lands on the bus → the screen comes up;
//   * a minute with no document → back to the clock;
//   * `PWR` → back to the clock now.
//
// The swipe still works — the navigator kept it — but nothing depends on it, and
// on a device with three of five screens missing it is not how anybody gets here.
//
// ## The screen nobody arrived at, and the overnight bug it caused
//
// Every rule above is about a **burst**, and the cue that ends one fires exactly
// once. That left the case where the screen goes up with no burst behind it: the
// carousel is still there, so a swipe reaches the limits when the stream has been
// quiet for hours, and `Tick` had nothing to say about a screen it had already
// taken away once. Nothing took it off again — so a board left overnight was
// found parked on last night's numbers with `4000 s ago` under them, drawn
// exactly like numbers from two seconds ago.
//
// Two answers, because the bug was two things:
//
//   * **the visit gets its own minute** (`Visited`). Not less than an arrival
//     gets: somebody who swiped here did it to read the last numbers. And not a
//     second timer over a live one — a screen the documents are holding up keeps
//     the burst's minute, or a swipe would cut a working session short.
//   * **the numbers say how old they are** (`Stale`, and `AgeText` below). The
//     age was already on the glass; what it lacked was units a person reads and
//     any mark saying the stream had stopped, both of which the console's own
//     `limits` readout has had all along.
//
// What that buys is a desk object that shows what the session is spending *while*
// there is a session, and a clock the rest of the time. §9.7 publishes on every
// render, so documents arrive every few seconds while Claude Code is working and
// stop dead when it is idle: the minute below is what turns that into "the screen
// follows the work".
//
// ## The one place the owner's words and the code differ
//
// "Back on `PWR` until the next document" cannot be taken literally: the next
// document is a few seconds away, so a dismissal would undo itself before the
// finger left the button. **So a dismissal lasts until the stream goes quiet** —
// the screen stays away until the minute expires and a document arrives after
// that. `PWR` therefore means "not for this session's burst", which is the only
// reading in which the button does anything at all.

#include <cstddef>
#include <cstdint>

#include "navigator.h"

namespace ui {

// §9.7's fields, bounded. Everything off the bus is attacker-shaped (§10.10), and
// this subject is as open as the other one.
inline constexpr size_t kModelNameSize = 40;
inline constexpr size_t kEffortSize = 12;
inline constexpr size_t kSessionCwdSize = 120;
inline constexpr size_t kResetsTextSize = 12;

// One gauge — a percentage, and for the two rate-limit windows a reset. **Absent
// is a state**, not a zero: §9.7 omits a section entirely for an API key rather
// than a subscription, and a bar drawn at 0 % would say the opposite of what is
// true.
struct Gauge {
    bool present = false;

    // Clamped 0..100 on the way in, a second time after the publisher already did
    // it — a bar drawn from somebody else's 130 overflows its track (§10.8.3).
    uint8_t used_percent = 0;

    // Unix seconds, or 0 for a gauge that has no reset. The context window is the
    // second kind: it resets when the session does, not on a clock.
    int64_t resets_at = 0;

    // The publisher's own resolution of the same thing, for a device whose clock
    // is not trustworthy yet (§10.8.3).
    int32_t resets_in = 0;
    char resets_in_text[kResetsTextSize] = {};
};

// One §9.7 document, reduced to what this screen shows. `line` is deliberately
// **not** here: it is the publisher's own rendering, in box-drawing characters
// this firmware has no glyphs for, and the numbers next to it are what this
// screen draws from.
struct Limits {
    int64_t ts = 0;
    char model[kModelNameSize] = {};
    char effort[kEffortSize] = {};

    // **Why the working directory is on this screen at all** (§10.8.3): every
    // Claude Code session on the machine publishes to one subject, so this is
    // whichever rendered last — not necessarily the session whose request is on
    // the card. Without this line the screen reads as belonging to that request.
    char cwd[kSessionCwdSize] = {};

    Gauge five_hour;
    Gauge seven_day;
    Gauge context;
};

// §9.2's traffic light, and there are two scales because the same percentage does
// not mean the same thing: half a five-hour window is an ordinary working state,
// half a context window is most of the way to a compact.
enum class Level : uint8_t {
    kGreen,
    kYellow,
    kRed,
};

// **Three implementations of these two scales now exist** — `render.rs`,
// `statusline.ts` and this — and each pins them in a test, including the
// assertion that they cannot drift into each other (§10.8.3).
Level WindowLevel(uint8_t used_percent);
Level ContextLevel(uint8_t used_percent);

// A port of `render.rs::countdown`, tested against the same cases: `now`, `<1m`,
// `59m`, `1h59m`, `4d8h`. Writes into the caller's buffer and never allocates.
void Countdown(int64_t seconds_left, char *out, size_t capacity);

// Room for `1193046h 12m`, which is what a `uint32_t` of seconds can reach, plus
// the terminator.
inline constexpr size_t kAgeTextSize = 16;

// **How long ago, in the units a person reads**: `3 s`, `66 m`, `8h 05m`. The
// same three bands `cli/console.cpp` has printed since it was written — and it
// prints them by calling this, so the age on the glass and the age in a pasted
// `limits` cannot say the same instant two different ways. `4000 s ago` is the
// reading this replaces, and the comment above that function is the argument:
// "21600 s ago is a conversion nobody should have to do in their head".
//
// Writes into the caller's buffer, never allocates, always terminates.
void AgeText(uint32_t seconds, char *out, size_t capacity);

class LimitsView {
   public:
    // §10.8.3's "connected means a document arrived recently". The owner's rule
    // is the same number doing a second job: after this long with nothing, the
    // screen goes.
    static constexpr uint32_t kQuietMs = 60000;

    // Below which a wall-clock time is not believable, which is the same call
    // §10.8.2 makes about the RTC. Before the clock is set, the countdown comes
    // from what the publisher resolved rather than from arithmetic on a wrong now.
    static constexpr int64_t kEarliestBelievableEpoch = 1704067200;  // 2024-01-01

    LimitsView() = default;
    LimitsView(const LimitsView &) = delete;
    LimitsView &operator=(const LimitsView &) = delete;

    // A document arrived. Returns true when this is the one that should raise the
    // screen — i.e. it was not dismissed, or the dismissal has expired.
    bool Arrived(const Limits &limits, uint32_t now_ms);

    // Returns true the moment the stream is judged to have stopped: a minute with
    // nothing. That is the screen's cue to leave, and it also **clears the
    // dismissal**, which is what makes `PWR` last exactly one burst.
    bool Tick(uint32_t now_ms);

    // The operator left the screen. Nothing arriving in the next minute brings it
    // back.
    void Dismissed();

    // **The operator arrived, by hand, at a screen no document raised.** Arms the
    // visit's own minute — but only while the stream is quiet: with documents
    // still coming the burst above owns the timer, and a second one would take
    // the screen away from a session that is still spending. See the header.
    void Visited(uint32_t now_ms);

    // The operator left it again, so nothing is armed. Without this the cue would
    // fire later at a screen that is no longer on the glass — harmless, and
    // exactly the kind of harmless nobody can reason about twice.
    void VisitEnded();
    bool Visiting() const { return visiting_; }

    bool HasDocument() const { return has_; }
    bool Quiet() const { return quiet_; }

    // **Older than the quiet window**: a snapshot rather than a reading, and the
    // screen has to say so — `5h 2 %` from last night is drawn exactly like
    // `5h 2 %` from two seconds ago, and that is the half of the overnight bug
    // that survives being navigated away from. `ActivityView::Stale` is the same
    // call one line further down the same screen.
    bool Stale(uint32_t now_ms) const;
    bool DismissedNow() const { return dismissed_; }

    const Limits &Document() const { return limits_; }

    // How long ago it landed, in milliseconds — what the screen shows as an age.
    uint32_t AgeMs(uint32_t now_ms) const;

    // The countdown for a gauge, resolved the way §10.8.3 asks: from `resets_at`
    // when the device's own clock is believable, and otherwise from what the
    // publisher worked out, aged by the time since it arrived.
    //
    // `epoch_now` is the device's idea of the time, or 0 when it has none.
    void CountdownFor(const Gauge &gauge, int64_t epoch_now, uint32_t now_ms, char *out,
                      size_t capacity) const;

    uint32_t Received() const { return received_; }

   private:
    static bool Reached(uint32_t now_ms, uint32_t at_ms) {
        return static_cast<int32_t>(now_ms - at_ms) >= 0;
    }

    Limits limits_ = {};
    bool has_ = false;
    bool quiet_ = true;
    bool dismissed_ = false;
    uint32_t arrived_ms_ = 0;
    uint32_t received_ = 0;
    bool visiting_ = false;
    uint32_t visit_ms_ = 0;
};

}  // namespace ui
