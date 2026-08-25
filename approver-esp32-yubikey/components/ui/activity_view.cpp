#include "activity_view.h"

namespace ui {
namespace {

// The separators, as constants because two files quote them: this one draws them
// and `test_activity.cpp` asserts them. **ASCII**, and that is the panel's font
// rather than a preference — `>` where the web page has `›` and `-` where it has
// `·`, because Montserrat's default subset has neither and LVGL draws a missing
// glyph as a box (§10.8.3).
constexpr char kAgentSeparator[] = " > ";
constexpr char kSummarySeparator[] = " - ";

// Appends what fits and returns the new end. The same shape `limits_view.cpp`'s
// helpers take: no `<cstdio>`, no format string, and nothing here can overrun.
size_t Append(const char *text, char *out, size_t capacity, size_t at) {
    while (*text != '\0' && at + 1 < capacity) {
        out[at++] = *text++;
    }
    return at;
}

}  // namespace

const char *ActivityEventText(ActivityEvent event) {
    switch (event) {
        case ActivityEvent::kPreTool:
            return "pre_tool";
        case ActivityEvent::kPostTool:
            return "post_tool";
        case ActivityEvent::kStop:
            return "stop";
    }
    return "unknown";
}

const char *ActivityStateText(ActivityState state) {
    switch (state) {
        case ActivityState::kRunning:
            return "running";
        case ActivityState::kThinking:
            return "thinking";
        case ActivityState::kIdle:
            return "idle";
    }
    return "unknown";
}

void ActivityView::Arrived(const Activity &activity, uint32_t now_ms) {
    activity_ = activity;
    has_ = true;
    arrived_ms_ = now_ms;
    ++received_;
}

uint32_t ActivityView::AgeMs(uint32_t now_ms) const {
    if (!has_) {
        return 0;
    }
    return now_ms - arrived_ms_;
}

bool ActivityView::Stale(uint32_t now_ms) const {
    if (!has_ || activity_.state == ActivityState::kIdle) {
        return false;
    }
    // The same wrap-safe comparison `LimitsView::Reached` makes: `now_ms` is a
    // 32-bit millisecond counter and it rolls over every 49 days.
    return static_cast<int32_t>(now_ms - (arrived_ms_ + kStaleMs)) >= 0;
}

void ActivityView::Headline(char *out, size_t capacity) const {
    if (out == nullptr || capacity == 0) {
        return;
    }
    out[0] = '\0';
    if (!has_) {
        return;
    }

    // **Only a tool that is *running* is named**, and everything else is the
    // state's own word - every `stop` document, every `post_tool`, and anything
    // that arrives with no tool in it at all.
    //
    // The `post_tool` half is the decision worth arguing over, because that
    // document does carry the tool that just returned and drawing it costs
    // nothing. But `Edit - main.cpp` for a session that finished editing a minute
    // ago cannot be told from the same line for one that is editing now, and a
    // readout that cannot be distinguished from a true one is the one thing this
    // line must not be (§10.8.3). `thinking` is less text and more information.
    //
    // The alternative for the no-tool case was an empty line, and a screen with a
    // blank row on it says less than one that says `idle`.
    if (activity_.state != ActivityState::kRunning || activity_.tool[0] == '\0') {
        size_t at = Append(ActivityStateText(activity_.state), out, capacity, 0);
        out[at] = '\0';
        return;
    }

    size_t at = 0;
    if (activity_.agent[0] != '\0') {
        // Whose work it is, first: on a device that runs one session's errands,
        // "Explore is doing this" is the part that changes what the line means.
        at = Append(activity_.agent, out, capacity, at);
        at = Append(kAgentSeparator, out, capacity, at);
    }
    at = Append(activity_.tool, out, capacity, at);
    if (activity_.summary[0] != '\0') {
        at = Append(kSummarySeparator, out, capacity, at);
        at = Append(activity_.summary, out, capacity, at);
    }
    out[at] = '\0';
}

}  // namespace ui
