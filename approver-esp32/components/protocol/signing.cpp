#include "signing.h"

namespace protocol {
namespace {

// A field, checked against its bound before anything is written. `nullptr` is a
// refusal rather than an empty string: a request that arrived without a
// `session_id` is a request that cannot be answered, and signing bytes with a
// hole where one should be is the way to produce a signature the hook cannot
// reproduce and nobody can explain.
bool Fits(const char *text, size_t max, size_t *length) {
    if (text == nullptr) {
        return false;
    }
    size_t n = 0;
    while (text[n] != '\0') {
        if (n >= max) {
            return false;
        }
        ++n;
    }
    *length = n;
    return true;
}

bool Same(const char *a, const char *b) {
    while (*a != '\0' && *a == *b) {
        ++a;
        ++b;
    }
    return *a == *b;
}

}  // namespace

size_t AppendInt(int64_t value, char *out, size_t out_size) {
    // **Built from the negative side.** The obvious loop negates a negative and
    // divides, which is undefined for `INT64_MIN` — there is no positive
    // counterpart of it — and on a device that echoes somebody else's `ts` back
    // into a signature, "it works for every value we have seen" is not the
    // standard. Accumulating downwards has no such value.
    char digits[20];
    size_t n = 0;

    int64_t remaining = value;
    const bool negative = remaining < 0;
    if (!negative) {
        remaining = -remaining;
    }

    do {
        const int digit = static_cast<int>(-(remaining % 10));
        digits[n++] = static_cast<char>('0' + digit);
        remaining /= 10;
    } while (remaining != 0);

    const size_t total = n + (negative ? 1 : 0);
    if (total > out_size) {
        return 0;
    }

    size_t written = 0;
    if (negative) {
        out[written++] = '-';
    }
    while (n > 0) {
        out[written++] = digits[--n];
    }
    return written;
}

size_t DecisionSigningBytes(const Decision &decision, char *out, size_t out_size) {
    if (out == nullptr || out_size == 0) {
        return 0;
    }

    size_t session_id = 0;
    size_t nonce = 0;
    size_t tool_name = 0;
    size_t input_sha256 = 0;
    size_t behavior = 0;

    // **Every check before the first byte is written.** §10.7 states this rule
    // for `poweroff` and §10.8.2 for a bad date; here it is the difference
    // between a caller getting nothing and a caller getting half a message it
    // could sign. `Fits` on the behaviour as well, so the comparison below is
    // reading a bounded string.
    if (!Fits(decision.session_id, kSessionIdMax, &session_id) ||
        !Fits(decision.nonce, kNonceMax, &nonce) ||
        !Fits(decision.tool_name, kToolNameMax, &tool_name) ||
        !Fits(decision.input_sha256, kSha256HexMax, &input_sha256) ||
        !Fits(decision.behavior, sizeof(kBehaviorAllow), &behavior)) {
        return 0;
    }

    // A verdict this protocol has no word for is not signed. The allowlist is
    // two entries because §7 has two behaviours.
    if (!Same(decision.behavior, kBehaviorAllow) && !Same(decision.behavior, kBehaviorDeny)) {
        return 0;
    }

    // Assembled into a scratch buffer first, for the same reason: a caller that
    // ignores the 0 must not find its own buffer half-written.
    char scratch[kSigningBytesMax];
    size_t at = 0;

    auto text = [&](const char *value, size_t length) {
        for (size_t i = 0; i < length; ++i) {
            scratch[at++] = value[i];
        }
    };
    auto separator = [&]() { scratch[at++] = '\n'; };

    const size_t v_digits = AppendInt(decision.v, scratch, sizeof scratch);
    if (v_digits == 0) {
        return 0;
    }
    at = v_digits;

    separator();
    text(decision.session_id, session_id);
    separator();
    text(decision.nonce, nonce);
    separator();
    text(decision.tool_name, tool_name);
    separator();
    text(decision.input_sha256, input_sha256);
    separator();
    text(decision.behavior, behavior);
    separator();

    // `updated_input_sha256` — always empty (§10.2). The field is a position
    // between two separators and nothing else, which is exactly what this line
    // is: it writes no characters, and the separator after it is what makes the
    // field exist at all.
    separator();

    const size_t ts_digits = AppendInt(decision.ts, scratch + at, sizeof scratch - at);
    if (ts_digits == 0) {
        return 0;
    }
    at += ts_digits;

    separator();
    // `reason` — always empty, and last, which is why nothing follows it. §7 puts
    // the only free-text field at the tail so that a `\n` inside it stays
    // unambiguous; this device never puts anything there at all.

    if (at + 1 > out_size) {
        return 0;
    }
    for (size_t i = 0; i < at; ++i) {
        out[i] = scratch[i];
    }
    out[at] = '\0';
    return at;
}

}  // namespace protocol
