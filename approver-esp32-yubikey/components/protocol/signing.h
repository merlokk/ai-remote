#pragma once

// **The bytes a decision is signed over** (CLAUDE.md §10.2, §7), assembled to be
// identical to what `approver/protocol.py::signing_bytes` produces for the same
// request. If these two ever disagree by one character, every reply this device
// sends is rejected and Claude Code falls back to asking in its own terminal —
// which looks exactly like a device that is not answering. So this file is small
// on purpose, and the tests next to it are the tier that matters (§10.11).
//
// **It includes `<cstdint>` and `<cstddef>` and nothing else**, which puts the
// whole of §7's most load-bearing arithmetic under Unity with no board and no
// fake, along with the request card, the state ranking, the Wi-Fi policy, the
// internet check, the bus endpoint and the bus link.
//
// The layout, `\n`-joined and nothing else, is §7's:
//
//     v, session_id, nonce, tool_name, input_sha256, behavior,
//     updated_input_sha256, ts, reason
//
// Two of those nine are **not parameters here, and that is §10.2's whole
// argument for this firmware being small**:
//
//   * `updated_input_sha256` is always empty. It is the only field a responder
//     *originates*, and carrying it would force this device to reproduce
//     Python's canonical JSON — sorted keys, no whitespace, `ensure_ascii` — and
//     hash it identically, which is the trap that cost `approver-web` a section
//     of its own documentation. This device never hashes anything: it echoes
//     `input_sha256` as the string it arrived as, and leaves this one blank.
//   * `reason` is always empty — there is no keyboard, the same call
//     `responder_yubikey.py` makes (§8.7).
//
// A field that can only ever hold one value should not be an argument somebody
// could pass a different one to. They are constants below, and the tests assert
// their positions rather than their content.
//
// And the two integers are the other thing worth getting right, because getting
// them wrong is invisible until a signature is rejected: `v` and `ts` must
// render exactly as Python's `str(int)`. `ts` is echoed from the request as an
// `int64_t` and printed by the digit loop in the implementation — **never
// through a `double`**, which is what happens the moment somebody reaches for a
// float format, and never re-derived from this device's own clock.

#include <cstddef>
#include <cstdint>

namespace protocol {

// §7's protocol version. The device does not negotiate it; it echoes what the
// request carried, and this is what a request is expected to say.
inline constexpr int32_t kVersion = 1;

// The two spellings §7 has, and there is no third — which is why the request
// card's `Verdict` has two values and no timer can produce one (§10.10).
inline constexpr char kBehaviorAllow[] = "allow";
inline constexpr char kBehaviorDeny[] = "deny";

// Field bounds, deliberately this file's own rather than borrowed from
// `ui::Request`. This component includes nothing, and the wiring that puts the
// two together is where a `static_assert` ties them — the same rule
// `nats_link.cpp` follows for the reply subject.
inline constexpr size_t kSessionIdMax = 63;
inline constexpr size_t kNonceMax = 47;
inline constexpr size_t kToolNameMax = 31;
inline constexpr size_t kSha256HexMax = 64;

// The largest the assembled bytes can be: every field at its bound, the two
// integers at their widest, eight separators. Sized here so a caller can put the
// buffer in `.bss` and never think about it again (§10.14.1).
inline constexpr size_t kSigningBytesMax = 11    // v, as int32 with a sign
                                           + kSessionIdMax + kNonceMax + kToolNameMax +
                                           kSha256HexMax + 5  // "allow" / "deny"
                                           + 20               // ts, as int64 with a sign
                                           + 8                // the separators
                                           + 1;               // and a terminator

// What §7 signs, reduced to the fields this device actually carries. Pointers
// rather than buffers: the caller owns the request, and copying 200 bytes to
// describe it would be a second place for it to be wrong.
struct Decision {
    int32_t v = kVersion;
    int64_t ts = 0;

    const char *session_id = nullptr;
    const char *nonce = nullptr;
    const char *tool_name = nullptr;
    const char *input_sha256 = nullptr;

    // `kBehaviorAllow` or `kBehaviorDeny`. Anything else is refused rather than
    // signed — a verdict this protocol has no word for must not become bytes.
    const char *behavior = nullptr;
};

// Writes the signing bytes into `out` and returns their length, **without** a
// terminator counted — the signature is over bytes, and a stray NUL inside them
// is a signature nobody can reproduce. `out` is still NUL-terminated when it
// fits, because everything that debugs this wants to print it.
//
// Returns 0 and **writes nothing** when a field is missing, too long, or the
// buffer is too small. That is §10.10's fail-safe reaching this far down: no
// bytes means no signature means no reply, and the hook falls back to its own
// prompt. A half-assembled buffer that a caller might sign anyway would be the
// one failure mode here that is worse than not answering.
size_t DecisionSigningBytes(const Decision &decision, char *out, size_t out_size);

// The decimal spelling of an integer, exactly as Python's `str(int)` gives it —
// no sign for a positive number, no padding, no grouping, and no float anywhere
// in the path. Public because it is the half of the above most worth pinning on
// its own, `INT64_MIN` included.
//
// Returns the number of characters written, or 0 when they would not fit.
size_t AppendInt(int64_t value, char *out, size_t out_size);

}  // namespace protocol
