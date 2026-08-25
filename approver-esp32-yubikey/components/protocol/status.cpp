#include "status.h"

#include <cstring>

#include "cJSON.h"

namespace protocol {
namespace {

// The same 2^53 rule the other two files state: cJSON parses every number into a
// `double`, and past that integers stop being distinguishable. Here nothing is
// echoed into a signature, so an out-of-range `ts` is simply not believed rather
// than being a refusal with consequences.
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

// Text, **truncated rather than refused**, and this is the one place in the
// firmware where that is the right way round. §10.8.4 refuses a `tool_input` that
// does not fit because a shortened command is a command somebody approves by
// reflex; a shortened model name or working directory is a readout that is
// slightly less specific, and dropping the whole document over it would lose the
// numbers as well.
void CopyTruncated(const cJSON *item, char *out, size_t capacity) {
    out[0] = '\0';
    if (!cJSON_IsString(item) || item->valuestring == nullptr) {
        return;
    }
    size_t at = 0;
    const char *text = item->valuestring;
    while (text[at] != '\0' && at + 1 < capacity) {
        out[at] = text[at];
        ++at;
    }
    out[at] = '\0';
}

// A percentage, clamped 0..100 — a second time after the publisher already did it
// (§9.7). A bar drawn from somebody else's 130 overflows its track, and this
// subject is as open as the other one.
uint8_t ReadPercent(const cJSON *object, const char *name) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    if (!cJSON_IsNumber(item)) {
        return 0;
    }
    const double value = item->valuedouble;
    if (value <= 0.0) {
        return 0;
    }
    if (value >= 100.0) {
        return 100;
    }
    // Rounded rather than truncated: 65.7 % of a window is nearer 66 than 65, and
    // the publisher sends a float precisely because it has the resolution.
    return static_cast<uint8_t>(value + 0.5);
}

// One window. Present only when the object is there — §9.7 omits each of them
// independently, so "five_hour but no seven_day" is an ordinary document.
void ReadGauge(const cJSON *parent, const char *name, ui::Gauge *out, bool with_reset) {
    *out = ui::Gauge{};
    const cJSON *object = cJSON_GetObjectItemCaseSensitive(parent, name);
    if (!cJSON_IsObject(object)) {
        return;
    }
    out->present = true;
    out->used_percent = ReadPercent(object, "used_percentage");
    if (!with_reset) {
        return;
    }

    int64_t resets_at = 0;
    if (ReadInt64(cJSON_GetObjectItemCaseSensitive(object, "resets_at"), &resets_at)) {
        out->resets_at = resets_at;
    }
    int64_t resets_in = 0;
    if (ReadInt64(cJSON_GetObjectItemCaseSensitive(object, "resets_in"), &resets_in)) {
        // Clamped into the field's own range rather than cast: a value that came
        // in as a double bigger than an `int32_t` would otherwise wrap into a
        // countdown that runs backwards.
        if (resets_in < 0) {
            resets_in = 0;
        }
        if (resets_in > 0x7fffffffLL) {
            resets_in = 0x7fffffffLL;
        }
        out->resets_in = static_cast<int32_t>(resets_in);
    }
    CopyTruncated(cJSON_GetObjectItemCaseSensitive(object, "resets_in_text"), out->resets_in_text,
                  sizeof out->resets_in_text);
}

}  // namespace

bool ParseStatus(const char *json, size_t length, ui::Limits *out) {
    if (json == nullptr || out == nullptr || length == 0 || length > kMaxStatusBytes) {
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
    // cannot read leaves the last good one standing — which is what §10.8.3 asks
    // for, and the opposite of the approval path's "drop it and forget it".
    //
    // **A mutation that writes straight into `*out` survives the tests, and that
    // is a fact about this function rather than a gap** (§10.11). The only
    // required field is `ts` and it is read first, so after that line nothing can
    // fail: there is no input today that gets half-written and then refused. The
    // local is defence for the day somebody adds a second required field below —
    // at which point the hazard is real and this already covers it. Kept
    // deliberately, at the cost of about 300 bytes of stack, and written down so
    // the survivor is not rediscovered as a mystery.
    ui::Limits parsed;

    // **`ts` is the one required field.** §9.7 calls it the clock the countdowns
    // were resolved against, so a document without one has numbers nothing can be
    // said about — and it is also the cheapest way to tell a `status` document
    // from whatever else somebody publishes on an open subject.
    if (!ReadInt64(cJSON_GetObjectItemCaseSensitive(root, "ts"), &parsed.ts)) {
        return false;
    }

    const cJSON *model = cJSON_GetObjectItemCaseSensitive(root, "model");
    if (cJSON_IsObject(model)) {
        CopyTruncated(cJSON_GetObjectItemCaseSensitive(model, "display_name"), parsed.model,
                      sizeof parsed.model);
    }

    const cJSON *effort = cJSON_GetObjectItemCaseSensitive(root, "effort");
    if (cJSON_IsObject(effort)) {
        CopyTruncated(cJSON_GetObjectItemCaseSensitive(effort, "level"), parsed.effort,
                      sizeof parsed.effort);
    }

    CopyTruncated(cJSON_GetObjectItemCaseSensitive(root, "cwd"), parsed.cwd, sizeof parsed.cwd);

    const cJSON *rate_limits = cJSON_GetObjectItemCaseSensitive(root, "rate_limits");
    if (cJSON_IsObject(rate_limits)) {
        ReadGauge(rate_limits, "five_hour", &parsed.five_hour, true);
        ReadGauge(rate_limits, "seven_day", &parsed.seven_day, true);
    }
    // The context window is a sibling of `rate_limits`, not a member of it — the
    // payload shape §9.7 publishes, which is not the shape the *line* renders.
    ReadGauge(root, "context_window", &parsed.context, false);

    *out = parsed;
    return true;
}

}  // namespace protocol
