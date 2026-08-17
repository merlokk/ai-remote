// GENERATED FILE -- do not edit by hand.
//
// Produced by `approver-esp32/tools/make_vectors.py` from the Python
// implementation itself (CLAUDE.md §10.11 tier 2). Edit the generator, run it,
// and commit what it writes; `tests/test_esp32_vectors.py` fails if this file
// and today's Python disagree, so an edit made here is one somebody will have to
// undo.

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

// §7's decision bytes and §6's registration-reply bytes, as the Python side
// produces them. `test_vectors.cpp` runs the firmware's own assemblers against
// every case here; the other host suites use `FindDecision` so that a layout
// lives in exactly one place.

namespace vectors {

// One case of `approver/protocol.py::signing_bytes`. The fields are what goes
// into `protocol::Decision`; `signing_bytes` is what must come out.
struct DecisionVector {
    const char *name;
    int32_t v;
    const char *session_id;
    const char *nonce;
    const char *tool_name;
    const char *input_sha256;
    const char *behavior;
    int64_t ts;
    const char *signing_bytes;
    // **Carried rather than derived from the literal.** The signature is over
    // bytes, so the length is part of the expectation and not a property of a
    // terminator.
    size_t signing_length;
};

inline constexpr DecisionVector kDecisions[] = {
    {
        "allow-bash",
        1,
        "4f9a2c1e-77b3-4d0a-9f21-8c6e5b3a1d02",
        "Tm9uY2VOb25jZU5vbmNlTm9uY2VOb25jZU5vbmNlMTI=",
        "Bash",
        "e8b70996597e349ff8cb37b6cbe3ae52f6c64e27bfb3f41edf819c4817501f14",
        "allow",
        INT64_C(1737345600),
        "1\n"
        "4f9a2c1e-77b3-4d0a-9f21-8c6e5b3a1d02\n"
        "Tm9uY2VOb25jZU5vbmNlTm9uY2VOb25jZU5vbmNlMTI=\n"
        "Bash\n"
        "e8b70996597e349ff8cb37b6cbe3ae52f6c64e27bfb3f41edf819c4817501f14\n"
        "allow\n"
        "\n"
        "1737345600\n",
        172,
    },
    {
        "deny-negative-ts",
        1,
        "s",
        "n",
        "Write",
        "0000000000000000000000000000000000000000000000000000000000000000",
        "deny",
        INT64_C(-1),
        "1\n"
        "s\n"
        "n\n"
        "Write\n"
        "0000000000000000000000000000000000000000000000000000000000000000\n"
        "deny\n"
        "\n"
        "-1\n",
        86,
    },
    {
        "allow-max-ts",
        1,
        "s",
        "n",
        "Bash",
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
        "allow",
        INT64_C(9223372036854775807),
        "1\n"
        "s\n"
        "n\n"
        "Bash\n"
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\n"
        "allow\n"
        "\n"
        "9223372036854775807\n",
        103,
    },
    {
        "deny-min-ts",
        1,
        "s",
        "n",
        "Bash",
        "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
        "deny",
        INT64_MIN,
        "1\n"
        "s\n"
        "n\n"
        "Bash\n"
        "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff\n"
        "deny\n"
        "\n"
        "-9223372036854775808\n",
        103,
    },
    {
        "allow-at-every-bound",
        INT32_MIN,
        "sssssssssssssssssssssssssssssssssssssssssssssssssssssssssssssss",
        "nnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnn",
        "ttttttttttttttttttttttttttttttt",
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
        "allow",
        INT64_MIN,
        "-2147483648\n"
        "sssssssssssssssssssssssssssssssssssssssssssssssssssssssssssssss\n"
        "nnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnn\n"
        "ttttttttttttttttttttttttttttttt\n"
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\n"
        "allow\n"
        "\n"
        "-9223372036854775808\n",
        249,
    },
    {
        "allow-non-ascii-session",
        1,
        "\321\201\320\265\321\201\321\201\320\270\321\217-\303\251\344\270\255",
        "n",
        "Bash",
        "e8b70996597e349ff8cb37b6cbe3ae52f6c64e27bfb3f41edf819c4817501f14",
        "allow",
        INT64_C(1737345600),
        "1\n"
        "\321\201\320\265\321\201\321\201\320\270\321\217-\303\251\344\270\255\n"
        "n\n"
        "Bash\n"
        "e8b70996597e349ff8cb37b6cbe3ae52f6c64e27bfb3f41edf819c4817501f14\n"
        "allow\n"
        "\n"
        "1737345600\n",
        111,
    },
};

inline constexpr size_t kDecisionCount = sizeof kDecisions / sizeof kDecisions[0];

// One case of `approver/protocol.py::registration_reply_signing_bytes` (§6).
struct ReplyVector {
    const char *name;
    int32_t v;
    bool ok;
    const char *key_id;
    const char *nonce;
    int64_t ts;
    const char *error;
    const char *signing_bytes;
    size_t signing_length;
};

inline constexpr ReplyVector kRegistrationReplies[] = {
    {
        "ok",
        1,
        true,
        "approver-esp32",
        "Tm9uY2VOb25jZU5vbmNlTm9uY2VOb25jZU5vbmNlMTI=",
        INT64_C(1737345600),
        "",
        "registration-reply\n"
        "1\n"
        "true\n"
        "approver-esp32\n"
        "Tm9uY2VOb25jZU5vbmNlTm9uY2VOb25jZU5vbmNlMTI=\n"
        "1737345600\n",
        97,
    },
    {
        "rejected",
        1,
        false,
        "approver-esp32",
        "Tm9uY2VOb25jZU5vbmNlTm9uY2VOb25jZU5vbmNlMTI=",
        INT64_C(1737345600),
        "token unknown",
        "registration-reply\n"
        "1\n"
        "false\n"
        "approver-esp32\n"
        "Tm9uY2VOb25jZU5vbmNlTm9uY2VOb25jZU5vbmNlMTI=\n"
        "1737345600\n"
        "token unknown",
        111,
    },
    {
        "rejected-anonymous",
        1,
        false,
        "",
        "n",
        INT64_C(-1),
        "",
        "registration-reply\n"
        "1\n"
        "false\n"
        "\n"
        "n\n"
        "-1\n",
        33,
    },
    {
        "ok-max-ts",
        1,
        true,
        "approver-esp32",
        "Tm9uY2VOb25jZU5vbmNlTm9uY2VOb25jZU5vbmNlMTI=",
        INT64_C(9223372036854775807),
        "",
        "registration-reply\n"
        "1\n"
        "true\n"
        "approver-esp32\n"
        "Tm9uY2VOb25jZU5vbmNlTm9uY2VOb25jZU5vbmNlMTI=\n"
        "9223372036854775807\n",
        106,
    },
    {
        "rejected-at-every-bound",
        1,
        false,
        "kkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkk",
        "nnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnn",
        INT64_C(0),
        "eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee",
        "registration-reply\n"
        "1\n"
        "false\n"
        "kkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkk\n"
        "nnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnn\n"
        "0\n"
        "eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee",
        217,
    },
    {
        "rejected-error-with-a-separator",
        1,
        false,
        "approver-esp32",
        "n",
        INT64_C(1737345600),
        "line one\n"
        "line two",
        "registration-reply\n"
        "1\n"
        "false\n"
        "approver-esp32\n"
        "n\n"
        "1737345600\n"
        "line one\n"
        "line two",
        72,
    },
};

inline constexpr size_t kRegistrationReplyCount =
    sizeof kRegistrationReplies / sizeof kRegistrationReplies[0];

// Lookup by name, so a suite that wants one particular layout does not depend on
// the order the generator happened to emit them in. Null when there is no such
// case, which the caller asserts -- a renamed vector must fail loudly rather
// than quietly test nothing.
inline const DecisionVector *FindDecision(const char *name) {
    for (const auto &vector : kDecisions) {
        if (std::strcmp(vector.name, name) == 0) {
            return &vector;
        }
    }
    return nullptr;
}

}  // namespace vectors
