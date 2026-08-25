#include "registration.h"

#include <cstring>

#include "cJSON.h"
#include "signing.h"

namespace protocol {
namespace {

// A string field, bounded and copied. Everything off the bus is attacker-shaped
// (§10.10), so a field that does not fit is a refusal rather than a truncation —
// the rule §10.8.4 states about a `tool_input` and this file states about an
// `error` somebody chose.
bool CopyBounded(const cJSON *item, char *out, size_t capacity) {
    if (!cJSON_IsString(item) || item->valuestring == nullptr) {
        return false;
    }
    const size_t length = std::strlen(item->valuestring);
    if (length + 1 > capacity) {
        return false;
    }
    std::memcpy(out, item->valuestring, length + 1);
    return true;
}

// An absent optional string is empty rather than a failure: the handler omits
// `error` on success and `key_id` on some rejections, and both are signed as `""`
// — which is what `registration_reply_signing_bytes` does on the other side.
bool CopyOptional(const cJSON *item, char *out, size_t capacity) {
    if (item == nullptr || cJSON_IsNull(item)) {
        out[0] = '\0';
        return true;
    }
    return CopyBounded(item, out, capacity);
}

// **A JSON number is a double, and `ts` is an int64.** cJSON parses every number
// into `valuedouble`, which holds integers exactly only up to 2^53 — so a
// timestamp beyond that is silently rounded. Seconds since 1970 will not reach
// that until long after this device is scrap, but the check is here because the
// *failure* would be silent: a rounded `ts` reproduces different signing bytes
// and the signature simply stops verifying, with nothing to point at.
constexpr double kExactIntegerLimit = 9007199254740992.0;  // 2^53

bool ReadInt64(const cJSON *item, int64_t *out) {
    if (!cJSON_IsNumber(item)) {
        return false;
    }
    const double value = item->valuedouble;
    // **At the limit, not past it**, and the difference is the whole point: 2^53
    // and 2^53+1 parse to the *same* double, so from here they are indis-
    // tinguishable and no rule can accept one and refuse the other. Below 2^53
    // every integer is its own value; at it, ambiguity starts. The round trip
    // below cannot see this — it compares the double with itself — which is why
    // this line is not redundant, and a test that assumed the opposite is what
    // established it.
    if (value >= kExactIntegerLimit || value <= -kExactIntegerLimit) {
        return false;
    }
    // Truncation rather than rounding, so a value that is not an integer is
    // refused below rather than quietly becoming one.
    const int64_t whole = static_cast<int64_t>(value);
    if (static_cast<double>(whole) != value) {
        return false;
    }
    *out = whole;
    return true;
}

bool Same(const char *a, const char *b) {
    if (a == nullptr || b == nullptr) {
        return false;
    }
    return std::strcmp(a, b) == 0;
}

}  // namespace

bool ParseToken(const char *token, char *key_id, size_t key_id_size) {
    if (token == nullptr || key_id == nullptr || key_id_size == 0) {
        return false;
    }
    const char *dot = std::strchr(token, '.');
    if (dot == nullptr) {
        return false;
    }
    const size_t length = static_cast<size_t>(dot - token);
    if (length == 0 || length + 1 > key_id_size) {
        return false;
    }
    std::memcpy(key_id, token, length);
    key_id[length] = '\0';
    return true;
}

size_t BuildRegistrationRequest(const RegistrationRequest &request, char *out, size_t out_size) {
    if (out == nullptr || out_size == 0 || request.token == nullptr || request.key_id == nullptr ||
        request.pubkey == nullptr || request.nonce == nullptr) {
        return 0;
    }
    if (std::strlen(request.token) > kTokenMax || std::strlen(request.key_id) > kKeyIdMax ||
        std::strlen(request.pubkey) > kB64_32Max || std::strlen(request.nonce) > kB64_32Max) {
        return 0;
    }

    // Built in a scratch buffer and copied, so a refusal leaves the caller's
    // buffer alone — `signing.h`'s rule, and here it matters for a second reason:
    // a half-written request is a request that would be published.
    char scratch[kRequestMax];

    cJSON *root = cJSON_CreateObject();
    if (root == nullptr) {
        return 0;
    }
    cJSON_AddNumberToObject(root, "v", request.v);
    cJSON_AddStringToObject(root, "token", request.token);
    cJSON_AddStringToObject(root, "key_id", request.key_id);
    cJSON_AddStringToObject(root, "pubkey", request.pubkey);
    // §10.2: fixed, never varied. The allowlist pins the scheme on the verifying
    // side and nothing this device sends may choose one.
    cJSON_AddStringToObject(root, "key_type", "ed25519");
    cJSON_AddStringToObject(root, "nonce", request.nonce);
    cJSON_AddNumberToObject(root, "ts", static_cast<double>(request.ts));

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

size_t RegistrationReplySigningBytes(int32_t v, bool ok, const char *key_id, const char *nonce,
                                     int64_t ts, const char *error, char *out, size_t out_size) {
    if (out == nullptr || key_id == nullptr || nonce == nullptr || error == nullptr) {
        return 0;
    }

    const size_t key_id_length = std::strlen(key_id);
    const size_t nonce_length = std::strlen(nonce);
    const size_t error_length = std::strlen(error);
    if (key_id_length > kKeyIdMax || nonce_length > kB64_32Max || error_length > kErrorMax) {
        return 0;
    }

    char scratch[kReplySigningBytesMax];
    size_t at = 0;

    auto text = [&](const char *value, size_t length) {
        for (size_t i = 0; i < length; ++i) {
            scratch[at++] = value[i];
        }
    };
    auto separator = [&]() { scratch[at++] = '\n'; };

    text(kRegistrationReplyContext, sizeof(kRegistrationReplyContext) - 1);
    separator();

    const size_t v_digits = AppendInt(v, scratch + at, sizeof scratch - at);
    if (v_digits == 0) {
        return 0;
    }
    at += v_digits;
    separator();

    // Python's `"true" if ok else "false"` — the JSON spelling, not C's `1`/`0`,
    // and not anything a locale could touch.
    if (ok) {
        text("true", 4);
    } else {
        text("false", 5);
    }
    separator();

    text(key_id, key_id_length);
    separator();
    text(nonce, nonce_length);
    separator();

    const size_t ts_digits = AppendInt(ts, scratch + at, sizeof scratch - at);
    if (ts_digits == 0) {
        return 0;
    }
    at += ts_digits;
    separator();

    // `error` last, and nothing after it.
    text(error, error_length);

    if (at + 1 > out_size) {
        return 0;
    }
    for (size_t i = 0; i < at; ++i) {
        out[i] = scratch[i];
    }
    out[at] = '\0';
    return at;
}

const char *ReplyStatusText(ReplyStatus status) {
    switch (status) {
        case ReplyStatus::kVerified:
            return "signed by the registration handler";
        case ReplyStatus::kNotJson:
            return "the reply is not an object";
        case ReplyStatus::kBadVersion:
            return "the reply speaks a different protocol version";
        case ReplyStatus::kNoOk:
            return "the reply says neither yes nor no";
        case ReplyStatus::kNoServerKey:
            return "the reply carries no handler key - is the handler up to date?";
        case ReplyStatus::kWrongServerKey:
            return "signed by a different key than this device already trusts";
        case ReplyStatus::kNonceMismatch:
            return "the reply answers a different request - a replay?";
        case ReplyStatus::kNoTimestamp:
            return "the reply has no timestamp";
        case ReplyStatus::kBadFields:
            return "the reply has a field this device cannot read";
        case ReplyStatus::kTooLong:
            return "the reply is longer than anything this device will read";
        case ReplyStatus::kBadSignature:
            return "the signature does not verify - somebody else answered";
    }
    return "unknown";
}

ReplyStatus ParseRegistrationReply(const char *json, size_t length, const char *expected_nonce,
                                   const char *pinned_server_key, ReplyVerifier verify, void *user,
                                   RegistrationReply *out) {
    if (json == nullptr || expected_nonce == nullptr || verify == nullptr || out == nullptr) {
        return ReplyStatus::kBadFields;
    }

    cJSON *root = cJSON_ParseWithLength(json, length);
    if (root == nullptr) {
        return ReplyStatus::kNotJson;
    }
    // Everything below leaves through here, so the parse tree is freed on every
    // path — a leak in the one function that runs on an open subject would be a
    // denial of service anybody on the LAN could trigger.
    struct Guard {
        cJSON *root;
        ~Guard() { cJSON_Delete(root); }
    } guard{root};

    if (!cJSON_IsObject(root)) {
        return ReplyStatus::kNotJson;
    }

    // **The order below is §10.7's and is not an implementation detail.** Nothing
    // is copied into `out` until the signature has verified, so a caller that
    // ignores the return value still cannot act on an unsigned field.
    int64_t v = 0;
    if (!ReadInt64(cJSON_GetObjectItemCaseSensitive(root, "v"), &v) || v != 1) {
        return ReplyStatus::kBadVersion;
    }

    const cJSON *ok_item = cJSON_GetObjectItemCaseSensitive(root, "ok");
    if (!cJSON_IsBool(ok_item)) {
        return ReplyStatus::kNoOk;
    }
    const bool ok = cJSON_IsTrue(ok_item) != 0;

    char server_key[kB64_32Max + 1];
    if (!CopyBounded(cJSON_GetObjectItemCaseSensitive(root, "server_key"), server_key,
                     sizeof server_key) ||
        server_key[0] == '\0') {
        return ReplyStatus::kNoServerKey;
    }

    // **Pinning, and it is checked before the signature rather than after.** A
    // valid signature by a key this device does not trust is the takeover the pin
    // exists to stop, and calling it "a bad signature" afterwards would name the
    // wrong problem.
    if (pinned_server_key != nullptr && pinned_server_key[0] != '\0' &&
        !Same(pinned_server_key, server_key)) {
        return ReplyStatus::kWrongServerKey;
    }

    const cJSON *nonce_item = cJSON_GetObjectItemCaseSensitive(root, "nonce");
    if (!cJSON_IsString(nonce_item) || !Same(nonce_item->valuestring, expected_nonce)) {
        return ReplyStatus::kNonceMismatch;
    }

    int64_t ts = 0;
    if (!ReadInt64(cJSON_GetObjectItemCaseSensitive(root, "ts"), &ts)) {
        return ReplyStatus::kNoTimestamp;
    }

    char key_id[kKeyIdMax + 1];
    char error[kErrorMax + 1];
    if (!CopyOptional(cJSON_GetObjectItemCaseSensitive(root, "key_id"), key_id, sizeof key_id) ||
        !CopyOptional(cJSON_GetObjectItemCaseSensitive(root, "error"), error, sizeof error)) {
        return ReplyStatus::kBadFields;
    }

    const cJSON *sig_item = cJSON_GetObjectItemCaseSensitive(root, "sig");
    if (!cJSON_IsString(sig_item) || sig_item->valuestring == nullptr ||
        std::strlen(sig_item->valuestring) > kB64_64Max) {
        return ReplyStatus::kBadSignature;
    }

    char message[kReplySigningBytesMax];
    const size_t message_length = RegistrationReplySigningBytes(
        static_cast<int32_t>(v), ok, key_id, expected_nonce, ts, error, message, sizeof message);
    if (message_length == 0) {
        return ReplyStatus::kTooLong;
    }

    if (!verify(server_key, message, message_length, sig_item->valuestring, user)) {
        return ReplyStatus::kBadSignature;
    }

    out->ok = ok;
    out->ts = ts;
    std::memcpy(out->key_id, key_id, std::strlen(key_id) + 1);
    std::memcpy(out->server_key, server_key, std::strlen(server_key) + 1);
    std::memcpy(out->error, error, std::strlen(error) + 1);
    return ReplyStatus::kVerified;
}

}  // namespace protocol
