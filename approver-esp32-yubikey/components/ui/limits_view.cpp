#include "limits_view.h"

namespace ui {
namespace {

// §9.2's numbers, and they are the *inclusive* upper edge of each band — 50 % of a
// rate-limit window is still green, 51 % is not. Written as constants rather than
// inline so a test can name them.
constexpr uint8_t kWindowGreen = 50;
constexpr uint8_t kWindowYellow = 80;
constexpr uint8_t kContextGreen = 20;
constexpr uint8_t kContextYellow = 45;

Level Band(uint8_t used, uint8_t green, uint8_t yellow) {
    if (used <= green) {
        return Level::kGreen;
    }
    if (used <= yellow) {
        return Level::kYellow;
    }
    return Level::kRed;
}

// Digits into a buffer, without `<cstdio>`. The same shape `protocol::AppendInt`
// takes and for the same reason: nothing here goes through a format string, and
// nothing here can overrun.
size_t AppendUnsigned(uint64_t value, char *out, size_t capacity, size_t at) {
    char digits[20];
    size_t n = 0;
    do {
        digits[n++] = static_cast<char>('0' + static_cast<int>(value % 10));
        value /= 10;
    } while (value != 0);

    while (n > 0 && at + 1 < capacity) {
        out[at++] = digits[--n];
    }
    return at;
}

void Copy(const char *text, char *out, size_t capacity) {
    size_t at = 0;
    while (text[at] != '\0' && at + 1 < capacity) {
        out[at] = text[at];
        ++at;
    }
    out[at] = '\0';
}

}  // namespace

Level WindowLevel(uint8_t used_percent) { return Band(used_percent, kWindowGreen, kWindowYellow); }

Level ContextLevel(uint8_t used_percent) {
    return Band(used_percent, kContextGreen, kContextYellow);
}

void Countdown(int64_t seconds_left, char *out, size_t capacity) {
    if (out == nullptr || capacity == 0) {
        return;
    }
    // `render.rs` saturates at zero rather than underflowing, and a reset in the
    // past reads `now` (§9.3). Same here, and it is the case that actually
    // happens: a document that arrived before the window turned over.
    const uint64_t seconds = seconds_left > 0 ? static_cast<uint64_t>(seconds_left) : 0;

    if (seconds == 0) {
        Copy("now", out, capacity);
        return;
    }
    if (seconds < 60) {
        Copy("<1m", out, capacity);
        return;
    }

    size_t at = 0;
    if (seconds < 3600) {
        at = AppendUnsigned(seconds / 60, out, capacity, at);
        if (at + 1 < capacity) {
            out[at++] = 'm';
        }
    } else if (seconds < 86400) {
        at = AppendUnsigned(seconds / 3600, out, capacity, at);
        if (at + 1 < capacity) {
            out[at++] = 'h';
        }
        at = AppendUnsigned((seconds % 3600) / 60, out, capacity, at);
        if (at + 1 < capacity) {
            out[at++] = 'm';
        }
    } else {
        at = AppendUnsigned(seconds / 86400, out, capacity, at);
        if (at + 1 < capacity) {
            out[at++] = 'd';
        }
        at = AppendUnsigned((seconds % 86400) / 3600, out, capacity, at);
        if (at + 1 < capacity) {
            out[at++] = 'h';
        }
    }
    out[at] = '\0';
}

void AgeText(uint32_t seconds, char *out, size_t capacity) {
    if (out == nullptr || capacity == 0) {
        return;
    }
    size_t at = 0;

    // Under ninety seconds the seconds *are* the answer, which is what a live
    // stream shows: `3 s ago` on a screen refreshed every hundred milliseconds.
    if (seconds < 90) {
        at = AppendUnsigned(seconds, out, capacity, at);
        if (at + 1 < capacity) {
            out[at++] = ' ';
        }
        if (at + 1 < capacity) {
            out[at++] = 's';
        }
        out[at] = '\0';
        return;
    }

    const uint32_t minutes = seconds / 60;
    if (minutes < 90) {
        at = AppendUnsigned(minutes, out, capacity, at);
        if (at + 1 < capacity) {
            out[at++] = ' ';
        }
        if (at + 1 < capacity) {
            out[at++] = 'm';
        }
        out[at] = '\0';
        return;
    }

    at = AppendUnsigned(minutes / 60, out, capacity, at);
    if (at + 1 < capacity) {
        out[at++] = 'h';
    }
    if (at + 1 < capacity) {
        out[at++] = ' ';
    }
    // Two digits, padded: `8h 5m` and `8h 05m` are the same duration, and only
    // one of them lines up with the one above it when the screen redraws.
    const uint32_t rest = minutes % 60;
    if (rest < 10 && at + 1 < capacity) {
        out[at++] = '0';
    }
    at = AppendUnsigned(rest, out, capacity, at);
    if (at + 1 < capacity) {
        out[at++] = 'm';
    }
    out[at] = '\0';
}

bool LimitsView::Arrived(const Limits &limits, uint32_t now_ms) {
    limits_ = limits;
    has_ = true;
    arrived_ms_ = now_ms;
    ++received_;

    const bool was_quiet = quiet_;
    quiet_ = false;

    // **A document takes a hand visit over.** The stream's minute is the live one
    // from here, and leaving the visit armed would take the screen away from a
    // session that had only just started spending.
    visiting_ = false;

    // **A dismissal survives the burst it was made in and nothing more.** Coming
    // out of quiet is a new burst, so the dismissal is spent; while documents are
    // still flowing it stands, which is the whole reason `PWR` does anything.
    if (was_quiet) {
        dismissed_ = false;
    }
    if (dismissed_) {
        return false;
    }
    return true;
}

bool LimitsView::Tick(uint32_t now_ms) {
    // **The visit's own minute, and it is checked first** because it only exists
    // in the state the burst below has nothing to say about: `quiet_` is already
    // true, so every line under this one would return false and the screen would
    // stay up for as long as the device had power (the header's overnight bug).
    if (visiting_ && Reached(now_ms, visit_ms_ + kQuietMs)) {
        visiting_ = false;
        return true;
    }
    if (!has_ || quiet_) {
        return false;
    }
    if (!Reached(now_ms, arrived_ms_ + kQuietMs)) {
        return false;
    }
    quiet_ = true;
    // Cleared here rather than in `Arrived`, so that the flag means "the operator
    // said no to *these* numbers" for exactly as long as those numbers keep
    // coming.
    dismissed_ = false;
    return true;
}

void LimitsView::Dismissed() { dismissed_ = true; }

void LimitsView::Visited(uint32_t now_ms) {
    if (!quiet_) {
        // Documents are still coming, so the screen is theirs and so is the
        // timer. Cleared rather than left alone: a swipe out and back in during
        // one burst must not leave a stale minute armed behind it.
        visiting_ = false;
        return;
    }
    visiting_ = true;
    visit_ms_ = now_ms;
}

void LimitsView::VisitEnded() { visiting_ = false; }

bool LimitsView::Stale(uint32_t now_ms) const {
    // Nothing has ever arrived: not stale, because there is nothing on the screen
    // to disbelieve. `Apply` draws no numbers at all in that state.
    return has_ && Reached(now_ms, arrived_ms_ + kQuietMs);
}

uint32_t LimitsView::AgeMs(uint32_t now_ms) const {
    if (!has_) {
        return 0;
    }
    return now_ms - arrived_ms_;
}

void LimitsView::CountdownFor(const Gauge &gauge, int64_t epoch_now, uint32_t now_ms, char *out,
                              size_t capacity) const {
    if (out == nullptr || capacity == 0) {
        return;
    }
    if (!gauge.present || gauge.resets_at == 0) {
        // The context window has no reset on a clock, and an absent gauge has
        // nothing to say. Empty rather than `now`, which would be a countdown that
        // has run out.
        out[0] = '\0';
        return;
    }

    // **After SNTP, arithmetic; before it, what the publisher worked out** — the
    // rule §10.8.3 states, and the reason `resets_in` travels on the wire at all.
    // A device whose clock is wrong by hours would otherwise print a countdown
    // wrong by hours, with nothing to say it was.
    if (epoch_now >= kEarliestBelievableEpoch) {
        Countdown(gauge.resets_at - epoch_now, out, capacity);
        return;
    }

    // Aged by the time since it arrived, so it still ticks down on a device with
    // no idea what time it is.
    const int64_t elapsed = static_cast<int64_t>(AgeMs(now_ms) / 1000);
    Countdown(static_cast<int64_t>(gauge.resets_in) - elapsed, out, capacity);
}

}  // namespace ui
