#include "activity.h"

#include <cstring>

#include "cJSON.h"

namespace protocol {
namespace {

// The 2^53 rule every file in this component states: cJSON parses each number
// into a `double`, and past that integers stop being distinguishable. Nothing
// here is echoed into a signature, so an out-of-range `ts` is simply not believed.
constexpr double kExactIntegerLimit = 9007199254740992.0;

bool ReadInt64(const cJSON *item, int64_t *out) {
    if (!cJSON_IsNumber(item)) {
        return false;
    }
    const double value = item->valuedouble;
    if (value >= kExactIntegerLimit || value <= -kExactIntegerLimit) {
        return false;
    }
    *out = static_cast<int64_t>(value);
    return true;
}

// Text, **truncated rather than refused** — `status.cpp` argues why that is the
// right way round for a readout and the wrong way round for a `tool_input`.
//
// The one thing this does that `status.cpp` does not: it **backs off to a UTF-8
// boundary**. §9.10 cuts the summary to 80 *characters*, which is up to four
// bytes each, so a path with Cyrillic in it arrives longer than the field —
// and a byte-counted cut through the middle of a sequence draws as a placeholder
// box next to a perfectly good line of text. Cheaper to end one character early.
void CopyTruncatedUtf8(const cJSON *item, char *out, size_t capacity) {
    out[0] = '\0';
    if (!cJSON_IsString(item) || item->valuestring == nullptr || capacity == 0) {
        return;
    }
    const char *text = item->valuestring;
    size_t at = 0;
    while (text[at] != '\0' && at + 1 < capacity) {
        out[at] = text[at];
        ++at;
    }

    // Only when something was actually cut: a string that fitted is already whole,
    // and walking back into it would drop a character nobody asked us to.
    if (text[at] != '\0') {
        // A continuation byte is `10xxxxxx`; a lead byte is anything else. Walk
        // back over the tail of a sequence that lost its end, at most three bytes
        // — the longest UTF-8 sequence is four.
        size_t back = 0;
        while (at > 0 && back < 3 && (static_cast<unsigned char>(out[at - 1]) & 0xC0) == 0x80) {
            --at;
            ++back;
        }
        // And over the lead byte itself, which is now the one whose continuations
        // were dropped. A lone lead byte is as much of a box as a lone
        // continuation.
        if (at > 0 && (static_cast<unsigned char>(out[at - 1]) & 0x80) != 0) {
            --at;
        }
    }
    out[at] = '\0';
}

// A word off the wire, matched against the set this firmware knows. **Refused
// rather than kept as text** (`activity.h`): everything downstream then takes an
// enum and no screen has to decide what to draw for a word it has never seen.
bool ReadEvent(const cJSON *item, ui::ActivityEvent *out) {
    if (!cJSON_IsString(item) || item->valuestring == nullptr) {
        return false;
    }
    const struct {
        const char *name;
        ui::ActivityEvent event;
    } kEvents[] = {
        {"pre_tool", ui::ActivityEvent::kPreTool},
        {"post_tool", ui::ActivityEvent::kPostTool},
        {"stop", ui::ActivityEvent::kStop},
    };
    for (const auto &entry : kEvents) {
        if (std::strcmp(item->valuestring, entry.name) == 0) {
            *out = entry.event;
            return true;
        }
    }
    return false;
}

bool ReadState(const cJSON *item, ui::ActivityState *out) {
    if (!cJSON_IsString(item) || item->valuestring == nullptr) {
        return false;
    }
    const struct {
        const char *name;
        ui::ActivityState state;
    } kStates[] = {
        {"running", ui::ActivityState::kRunning},
        {"thinking", ui::ActivityState::kThinking},
        {"idle", ui::ActivityState::kIdle},
    };
    for (const auto &entry : kStates) {
        if (std::strcmp(item->valuestring, entry.name) == 0) {
            *out = entry.state;
            return true;
        }
    }
    return false;
}

}  // namespace

bool ParseActivity(const char *json, size_t length, ui::Activity *out) {
    if (json == nullptr || out == nullptr || length == 0 || length > kMaxActivityBytes) {
        return false;
    }

    cJSON *root = cJSON_ParseWithLength(json, length);
    if (root == nullptr) {
        return false;
    }
    struct Guard {
        cJSON *root;
        ~Guard() { cJSON_Delete(root); }
    } guard{root};

    if (!cJSON_IsObject(root)) {
        return false;
    }

    // Assembled locally and copied out at the end, so a document this device
    // cannot read leaves the last good one standing (§10.8.3). **Here that local
    // is load-bearing rather than defensive**, which is the difference from
    // `ParseStatus`: there are four required fields below and three of them are
    // read after the first, so a document that fails on `state` has already had
    // `ts` and `event` read successfully — writing straight into `*out` would
    // leave half of somebody else's message on the screen.
    ui::Activity parsed;

    // **`v` first, and it is the "this is ours" test.** §9.7's document is
    // recognisable by always carrying `ts` and `line`; this one has no such pair,
    // so the version is what tells one of ours from anything else on an open
    // subject — and a `v: 2` from a newer publisher is refused rather than
    // half-understood.
    int64_t version = 0;
    if (!ReadInt64(cJSON_GetObjectItemCaseSensitive(root, "v"), &version) ||
        version != kActivityVersion) {
        return false;
    }
    if (!ReadInt64(cJSON_GetObjectItemCaseSensitive(root, "ts"), &parsed.ts)) {
        return false;
    }
    if (!ReadEvent(cJSON_GetObjectItemCaseSensitive(root, "event"), &parsed.event)) {
        return false;
    }
    if (!ReadState(cJSON_GetObjectItemCaseSensitive(root, "state"), &parsed.state)) {
        return false;
    }

    // Everything from here is optional, exactly as §9.10 sends it: absent is
    // absent, and a `stop` document has none of it.
    CopyTruncatedUtf8(cJSON_GetObjectItemCaseSensitive(root, "tool_name"), parsed.tool,
                      sizeof parsed.tool);
    CopyTruncatedUtf8(cJSON_GetObjectItemCaseSensitive(root, "summary"), parsed.summary,
                      sizeof parsed.summary);
    CopyTruncatedUtf8(cJSON_GetObjectItemCaseSensitive(root, "agent_type"), parsed.agent,
                      sizeof parsed.agent);

    *out = parsed;
    return true;
}

}  // namespace protocol
