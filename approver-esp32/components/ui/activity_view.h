#pragma once

// What Claude is *doing*, as one line (CLAUDE.md §10.8.3; `statusline/CLAUDE.md`
// §9.10).
//
// **This file includes `<cstdint>` and `<cstddef>` and nothing else**, so all of
// it runs under Unity with no board (§10.11) — the same reason `limits_view.h`
// next door is shaped the way it is. The pixels are `screens/limits_screen.cpp`'s.
//
// ## Why it is a passenger on the limits screen and not a screen of its own
//
// §9.10's documents and §9.7's come from the same binary and the same session:
// one on every render, one on every tool call. The limits screen already arrives
// when the numbers do and leaves a minute after they stop (`limits_view.h`), and
// that window is exactly the window in which "what is it doing" is worth reading.
// So this holds a document and answers questions about it, and **owns no arrival
// rule, no quiet timer and no dismissal** — there is one screen with one of each,
// and a second publisher racing to raise and drop it would make both untestable.
//
// The cost is stated rather than hidden: with `activity: false` or `subject`
// renamed in `statusline-config.json` (§9.9) there would be no numbers, no screen
// and therefore no line, however many activity documents arrived. That is the
// trade, and the alternative — this raising a screen of its own — is a bigger
// change to §10.8's navigation than the line is worth today.
//
// ## The state is carried by colour, not by a word
//
// The web page has room for `running` beside the headline; a 28-point line on a
// 480-pixel panel has room for about twenty-eight characters, and spending nine
// of them on a word the colour already says is the wrong trade (§10.8.3 makes the
// same call about the percentage). So `running` and `thinking` are bright,
// `idle` is faint, and the text is the tool and what it is doing.

#include <cstddef>
#include <cstdint>

namespace ui {

// §9.10's fields, bounded — everything off the bus is attacker-shaped (§10.10),
// and this subject is as open as the other two.
//
// **48 for the tool name is not generosity.** `Bash` is four characters, but an
// MCP tool is called `mcp__claude_ai_Atlassian__searchConfluenceUsingCql`, and a
// readout that shows half of that says less than one that shows all of it.
inline constexpr size_t kActivityToolSize = 48;
inline constexpr size_t kActivityAgentSize = 32;

// The publisher cuts the summary to 80 *characters* (§9.10), which is up to four
// bytes each. 128 is the compromise: every ASCII summary arrives whole, and a
// path with Cyrillic in it arrives truncated rather than refused — on a UTF-8
// boundary, because half a character is a placeholder glyph on the panel.
inline constexpr size_t kActivitySummarySize = 128;

// Tool, separator, agent, separator, summary, terminator — the longest line
// `Headline` can produce, so a caller never has to guess.
inline constexpr size_t kActivityHeadlineSize =
    kActivityAgentSize + 3 + kActivityToolSize + 3 + kActivitySummarySize + 1;

// §9.10's three events. Kept as an enum rather than the string off the wire: a
// name this firmware does not know is refused by the parser, so by the time a
// document is here it is one of these three.
enum class ActivityEvent : uint8_t {
    kPreTool,
    kPostTool,
    kStop,
};

// And the state each implies. `post_tool` is `thinking` and deliberately not
// `idle`: the tool is done, the turn is not, and the only event that means
// "waiting for a human" is `stop`.
enum class ActivityState : uint8_t {
    kRunning,
    kThinking,
    kIdle,
};

const char *ActivityEventText(ActivityEvent event);
const char *ActivityStateText(ActivityState state);

// One §9.10 document, reduced to what this line shows. `session_id`, `cwd` and
// `tool_use_id` are **not** here: the screen already carries the session's
// directory from §9.7's document, and the id exists to pair a `post_tool` with
// its `pre_tool` — which is a subscriber's job and not a readout's.
struct Activity {
    int64_t ts = 0;
    ActivityEvent event = ActivityEvent::kStop;
    ActivityState state = ActivityState::kIdle;

    // Empty on `stop`, which is not about a tool.
    char tool[kActivityToolSize] = {};
    // The one value the publisher lifted out of `tool_input`, already flattened
    // and cut (§9.10). Empty for a tool whose input has nothing worth quoting.
    char summary[kActivitySummarySize] = {};
    // Set only inside a subagent, so the line can say whose work this is.
    char agent[kActivityAgentSize] = {};
};

class ActivityView {
   public:
    // **Ten minutes, where the numbers get one** (`LimitsView::kQuietMs`), and the
    // difference is what the two publishers do. §9.7 publishes on every render,
    // so silence there means something stopped; §9.10 publishes on tool calls, so
    // a session thinking hard — or parked on a permission request this device is
    // showing on a card — legitimately says nothing for minutes. Ten is where
    // "running Bash" stops being believable.
    static constexpr uint32_t kStaleMs = 600000;

    ActivityView() = default;
    ActivityView(const ActivityView &) = delete;
    ActivityView &operator=(const ActivityView &) = delete;

    // A document arrived. Returns nothing worth a decision: raising a screen is
    // `LimitsView`'s job, and this line is drawn on whatever is already up.
    void Arrived(const Activity &activity, uint32_t now_ms);

    bool HasDocument() const { return has_; }
    const Activity &Document() const { return activity_; }
    uint32_t Received() const { return received_; }
    uint32_t AgeMs(uint32_t now_ms) const;

    // Past [`kStaleMs`], and never for an `idle` document: "idle" stays true until
    // something else happens, and fading it would suggest the session vanished
    // when it is simply done.
    bool Stale(uint32_t now_ms) const;

    // The line: `Bash - py -m pytest -q`, `Explore > Grep - TODO`, or the state's
    // own word for a document with no tool in it. Never allocates, always
    // terminated, and **ASCII only** — the panel's font is Montserrat's default
    // subset, so a `·` or a `›` would draw as a placeholder box.
    void Headline(char *out, size_t capacity) const;

   private:
    Activity activity_ = {};
    bool has_ = false;
    uint32_t arrived_ms_ = 0;
    uint32_t received_ = 0;
};

}  // namespace ui
