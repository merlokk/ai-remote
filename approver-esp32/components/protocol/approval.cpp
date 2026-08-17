#include "approval.h"

#include <cstring>

#include "cJSON.h"
#include "registration.h"
#include "signing.h"

namespace protocol {
namespace {

// The bounds this file checks against are `signing.h`'s, and the buffers it
// writes into are `ui::Request`'s. They have to agree, and this is where that is
// asserted rather than assumed — the two live in different components on purpose
// (§10.14.2) and nothing else would notice one of them moving.
static_assert(kSessionIdMax + 1 == ui::kSessionIdSize, "session_id bounds disagree");
static_assert(kNonceMax + 1 == ui::kNonceSize, "nonce bounds disagree");
static_assert(kToolNameMax + 1 == ui::kToolNameSize, "tool_name bounds disagree");
static_assert(kSha256HexMax + 1 == ui::kSha256HexSize, "input_sha256 bounds disagree");

// Same limit and same reason as the registration reply's: cJSON parses every
// number into a `double`, so at 2^53 integers stop being distinguishable and a
// `ts` echoed back would sign different bytes than the hook signed.
constexpr double kExactIntegerLimit = 9007199254740992.0;

bool ReadInt64(const cJSON *item, int64_t *out) {
    if (!cJSON_IsNumber(item)) {
        return false;
    }
    const double value = item->valuedouble;
    if (value >= kExactIntegerLimit || value <= -kExactIntegerLimit) {
        return false;
    }
    const int64_t whole = static_cast<int64_t>(value);
    if (static_cast<double>(whole) != value) {
        return false;
    }
    *out = whole;
    return true;
}

// Required text: present, a string, and short enough. Absent and too long are
// different answers because they are different problems on the network.
RequestStatus CopyRequired(const cJSON *root, const char *name, char *out, size_t capacity) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
    if (!cJSON_IsString(item) || item->valuestring == nullptr) {
        return RequestStatus::kMissingField;
    }
    const size_t length = std::strlen(item->valuestring);
    if (length + 1 > capacity) {
        return RequestStatus::kTooLong;
    }
    std::memcpy(out, item->valuestring, length + 1);
    return RequestStatus::kOk;
}

// Optional text, and `cwd` is the only one: the hook sends it as `null` when it
// has none, and a card with no working directory is still a card worth answering.
RequestStatus CopyOptional(const cJSON *root, const char *name, char *out, size_t capacity) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
    if (item == nullptr || cJSON_IsNull(item)) {
        out[0] = '\0';
        return RequestStatus::kOk;
    }
    return CopyRequired(root, name, out, capacity);
}

}  // namespace

const char *RequestStatusText(RequestStatus status) {
    switch (status) {
        case RequestStatus::kOk:
            return "ok";
        case RequestStatus::kNotJson:
            return "not an object";
        case RequestStatus::kBadVersion:
            return "a protocol version this device does not speak";
        case RequestStatus::kMissingField:
            return "a field this device needs is missing";
        case RequestStatus::kTooLong:
            return "a field is longer than this device will show";
        case RequestStatus::kNoReply:
            return "nowhere to answer it";
        case RequestStatus::kBadTimestamp:
            return "a timestamp this device cannot echo exactly";
    }
    return "unknown";
}

RequestStatus ParseApprovalRequest(const char *json, size_t length, const char *reply_subject,
                                   ui::Request *out) {
    if (json == nullptr || out == nullptr) {
        return RequestStatus::kNotJson;
    }
    // **First, and before any parsing**: a request with nowhere to answer is not
    // a card. Checking it here rather than after means an attacker cannot make
    // this device spend a parse on a message it was always going to drop.
    if (reply_subject == nullptr || reply_subject[0] == '\0') {
        return RequestStatus::kNoReply;
    }
    if (std::strlen(reply_subject) + 1 > ui::kReplySubjectSize) {
        return RequestStatus::kTooLong;
    }

    cJSON *root = cJSON_ParseWithLength(json, length);
    if (root == nullptr) {
        return RequestStatus::kNotJson;
    }
    struct Guard {
        cJSON *root;
        ~Guard() { cJSON_Delete(root); }
    } guard{root};

    if (!cJSON_IsObject(root)) {
        return RequestStatus::kNotJson;
    }

    int64_t v = 0;
    if (!ReadInt64(cJSON_GetObjectItemCaseSensitive(root, "v"), &v) || v != kVersion) {
        return RequestStatus::kBadVersion;
    }

    // Assembled into a local and copied out at the end, so a refusal leaves the
    // caller's card untouched — the rule this file shares with `signing.cpp`, and
    // here it means a bad message cannot half-overwrite the request already on
    // the glass.
    ui::Request parsed;
    parsed.v = static_cast<int32_t>(v);

    if (!ReadInt64(cJSON_GetObjectItemCaseSensitive(root, "ts"), &parsed.ts)) {
        return RequestStatus::kBadTimestamp;
    }

    struct Field {
        const char *name;
        char *out;
        size_t capacity;
    };
    const Field required[] = {
        {"session_id", parsed.session_id, sizeof parsed.session_id},
        {"nonce", parsed.nonce, sizeof parsed.nonce},
        {"tool_name", parsed.tool_name, sizeof parsed.tool_name},
        {"input_sha256", parsed.input_sha256, sizeof parsed.input_sha256},
    };
    for (const Field &field : required) {
        const RequestStatus status = CopyRequired(root, field.name, field.out, field.capacity);
        if (status != RequestStatus::kOk) {
            return status;
        }
    }

    const RequestStatus cwd = CopyOptional(root, "cwd", parsed.cwd, sizeof parsed.cwd);
    if (cwd != RequestStatus::kOk) {
        return cwd;
    }

    // **`tool_input` is rendered back to compact JSON, whole.**
    //
    // The tempting alternative is to reach inside it — show `command` for a
    // `Bash` and drop the rest — and §10.8.4 is exactly the section that forbids
    // it: a card that shows part of what is being asked for is a card somebody
    // approves by reflex. One rule, no branch to get wrong, and what the operator
    // reads is the structure `input_sha256` was computed over.
    //
    // The cost is stated rather than hidden: a `Write` of a file larger than
    // `kToolInputSize` produces no card and no reply, and the question goes back
    // to Claude Code's own terminal (§7's timeout).
    const cJSON *tool_input = cJSON_GetObjectItemCaseSensitive(root, "tool_input");
    if (tool_input == nullptr) {
        return RequestStatus::kMissingField;
    }
    if (!cJSON_PrintPreallocated(const_cast<cJSON *>(tool_input), parsed.tool_input,
                                 static_cast<int>(sizeof parsed.tool_input), 0)) {
        return RequestStatus::kTooLong;
    }

    std::memcpy(parsed.reply, reply_subject, std::strlen(reply_subject) + 1);

    *out = parsed;
    return RequestStatus::kOk;
}

size_t BuildDecisionReply(const ui::Request &request, const char *behavior, const char *key_id,
                          const char *signature_b64, char *out, size_t out_size) {
    if (out == nullptr || out_size == 0 || behavior == nullptr || key_id == nullptr ||
        signature_b64 == nullptr) {
        return 0;
    }
    // The same allowlist `signing.h` applies, for the same reason: a verdict this
    // protocol has no word for must not reach the wire, even though the signature
    // that would have to cover it could not have been made.
    if (std::strcmp(behavior, kBehaviorAllow) != 0 && std::strcmp(behavior, kBehaviorDeny) != 0) {
        return 0;
    }

    char scratch[kDecisionReplyMax];

    cJSON *root = cJSON_CreateObject();
    if (root == nullptr) {
        return 0;
    }
    // **The six echoed fields are echoed, not recomputed** — `hook.py` compares
    // each of them against what it sent, and `ts` in particular is the request's
    // and never this device's clock (§10.2).
    cJSON_AddNumberToObject(root, "v", request.v);
    cJSON_AddStringToObject(root, "behavior", behavior);
    cJSON_AddStringToObject(root, "reason", kReasonNone);
    cJSON_AddStringToObject(root, "session_id", request.session_id);
    cJSON_AddStringToObject(root, "tool_name", request.tool_name);
    cJSON_AddStringToObject(root, "input_sha256", request.input_sha256);
    cJSON_AddStringToObject(root, "nonce", request.nonce);
    cJSON_AddNumberToObject(root, "ts", static_cast<double>(request.ts));
    cJSON_AddStringToObject(root, "key_id", key_id);
    cJSON_AddStringToObject(root, "sig", signature_b64);

    const cJSON_bool printed =
        cJSON_PrintPreallocated(root, scratch, static_cast<int>(sizeof scratch), 0);
    cJSON_Delete(root);
    if (!printed) {
        return 0;
    }

    const size_t length = std::strlen(scratch);
    if (length + 1 > out_size) {
        return 0;
    }
    std::memcpy(out, scratch, length + 1);
    return length;
}

}  // namespace protocol
