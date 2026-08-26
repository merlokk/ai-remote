#pragma once

// The request card — the queue the device exists for (CLAUDE.md §10.2), and the
// part of it that decides rather than shows.
//
// **This file includes `<cstdint>` and `<cstddef>` and nothing else**, so it runs
// under Unity with no board and no fake (§10.11) — and it is the file where that
// matters most, because every rule below is a rule about not approving something
// by accident.
//
// It is logic rather than library (§10.14.2), and it **does** know what an
// approval is: §7's fields are in `Request` verbatim, because the whole point of
// this object is to hold one until a human has answered it.
//
// The rules it enforces, and each is somebody's sentence made executable:
//
//   * **no press, no verdict** (§10.10). There is no timer that answers, no
//     default, and no third way out. `Tick` can make a card *disappear*; it can
//     never make one say `allow`;
//   * **a card that arrives under a finger is not a press** — a queued gesture,
//     in the shape a button has. A press counts only if it *began* at
//     least `kPressGuardMs` after the card appeared — one comparison, and it
//     covers both "the finger was already down" and "the card has only just
//     arrived", which are the same mistake seen from two ends;
//   * **expiry sends nothing and says so** (§10.10: failure is visible). A card
//     that timed out leaves a receipt reading as a timeout, not as a deny — the
//     hook has already fallen back to its own prompt and the operator needs to
//     know that happened;
//   * **a `tool_input` that does not fit is refused, never truncated** (§10.3): a
//     request whose command has not been fully seen is exactly the one people
//     approve by reflex. Refusing is §10.10's fail-safe — no reply, one log
//     line, the hook times out — and the cost is stated at `kToolInputSize`;
//   * **full is a designed state** (§10.14.1): the fifth arrival is refused, and
//     the caller's job is then the same fail-safe.
//
// What it deliberately does not do: hash anything, sign anything, or know what a
// subject is. It carries the reply subject as opaque text and hands the whole
// request back when a human has decided — the signer is somebody else's file, and
// §10.6 has not been written yet.

#include <cstddef>
#include <cstdint>


namespace ui {

// --- The bounds -----------------------------------------------------------
// One per §7 field, and every one of them is a refusal rather than a truncation.
// `+ 1` where the value is text with a known length, for the terminator.

// §7's `session_id` is Claude Code's own and is a UUID in practice.
inline constexpr size_t kSessionIdSize = 64;

// `nonce` is 32 random bytes in base64 — 44 characters.
inline constexpr size_t kNonceSize = 48;

// `input_sha256` is hex: 64 characters. Echoed as the string it arrived as
// (§10.2: the device never hashes anything).
inline constexpr size_t kSha256HexSize = 65;

inline constexpr size_t kToolNameSize = 32;

// `cwd` is a Windows path in practice, so it is the field most likely to be long
// and the one whose tail matters least — but it is still refused rather than cut,
// because a path that reads as a different project is worse than no card.
inline constexpr size_t kCwdSize = 160;

// **The one bound with a consequence worth knowing before it surprises
// somebody.** For `Bash` this holds the command with room to spare; for `Write`
// it holds the file being written, and a file larger than this **cannot be
// approved on the device** — the card is refused, nothing is replied, and §7's
// timeout puts the question back in Claude Code's own terminal. That is the same
// shape as the 64 KB server bound of §10.5: a limit that costs a fallback rather
// than a wrong answer. Raising it costs four times the number in static RAM.
inline constexpr size_t kToolInputSize = 2048;

// The inbox to answer into. Opaque here — the same rule `wifi_policy.h` follows
// about SSIDs, and `nats_link.cpp` is where a `static_assert` ties this to
// `nats::kSubjectSize`.
inline constexpr size_t kReplySubjectSize = 72;

// What arrives on `approvals.*` (§7), reduced to what a card needs and what a
// reply has to echo. Nothing derived and nothing hashed — §10.2's whole argument
// for this firmware being small.
struct Request {
    int32_t v = 0;
    int64_t ts = 0;

    char session_id[kSessionIdSize] = {};
    char nonce[kNonceSize] = {};
    char input_sha256[kSha256HexSize] = {};
    char tool_name[kToolNameSize] = {};
    char cwd[kCwdSize] = {};
    char tool_input[kToolInputSize] = {};

    // Where the decision goes. Empty is not a card: §10.10 says a request with
    // no reply-to is dropped rather than shown, because a press on it could
    // never reach anybody.
    char reply[kReplySubjectSize] = {};

    // How long the card may stay up. **The hook's number, not the device's** — it
    // must be at least the `timeout` in `handler-config.json`, and reaching zero
    // means the card goes with no reply.
    uint32_t ttl_ms = 0;
};

// §7 has two behaviours and this has two values. There is no third, which is
// why `Tick` cannot produce one.
enum class Verdict : uint8_t {
    kAllow,
    kDeny,
};

// What the card area is showing.
enum class CardState : uint8_t {
    kIdle,     // nothing pending
    kCard,     // a request is up and can be answered
    kReceipt,  // what happened to the last one, for a beat
};

// Why the last card went. `kTimedOut` is not a verdict and is deliberately in
// the same enum as the two that are: the receipt has to be able to say "nothing
// was sent" in the same place it would have said `allow`.
enum class Outcome : uint8_t {
    kNone,
    kAllowed,
    kDenied,
    kTimedOut,
};

class RequestCard {
   public:
    // **Four, and the number stands on its own argument**: four requests waiting
    // at once is already a session that has got ahead of its operator, and the
    // fifth is dropped rather than queued (§10.10 counts it). Four 2.3 KB slots is
    // also what it costs in `.bss`, which `build.md` prints.
    static constexpr uint8_t kMaxPending = 4;

    // Ignore presses for the first ~300 ms of any newly presented card, and
    // discard any press that began before it appeared. Both halves are one
    // comparison here — see `Press`.
    static constexpr uint32_t kPressGuardMs = 300;

    // How long the receipt stays up. Long enough to be a beat, short enough that
    // the next card is not kept waiting behind it.
    static constexpr uint32_t kReceiptMs = 2000;

    // Used when a request does not name its own. Below any plausible hook
    // timeout on purpose: a card that outlives the hook waiting for it is a card
    // whose press publishes into a dead inbox (§10.10).
    static constexpr uint32_t kDefaultTtlMs = 60000;

    RequestCard() = default;
    RequestCard(const RequestCard &) = delete;
    RequestCard &operator=(const RequestCard &) = delete;

    // False when the queue is full, when the payload does not fit, or when it
    // carries no reply subject. In every case the caller's job is §10.10's: drop
    // it, one log line, no reply. The lengths are checked here rather than by the
    // parser so that "it did not fit" is one rule in one place.
    bool Arrived(const Request &request, uint32_t now_ms);

    // Runs the timers. Returns true when something changed, which is the caller's
    // cue that the light may have to say something different — and the reason this
    // is not `void`.
    //
    // **It can drop a card and it can never answer one.**
    bool Tick(uint32_t now_ms);

    CardState State() const { return state_; }

    // The card being answered, or `nullptr`. Only ever the oldest.
    const Request *Front() const;

    uint8_t Pending() const { return count_; }

    // Everything except the one being answered.
    uint8_t Waiting() const { return count_ > 0 ? static_cast<uint8_t>(count_ - 1) : uint8_t{0}; }

    // Milliseconds left on the card, 0 when there is none or it is due.
    uint32_t RemainingMs(uint32_t now_ms) const;

    // A press, and `pressed_at_ms` is **when the finger went down** rather than
    // now. That is the whole guard: a press is taken only if it began at least
    // `kPressGuardMs` after the card appeared, which refuses both a finger that
    // was already down and a card that has only just arrived.
    //
    // Returns true when a decision was made, and fills `out` with the request it
    // is about — the caller then signs and publishes it (§7), or, until §10.6
    // exists, says that it cannot.
    bool Press(Verdict verdict, uint32_t pressed_at_ms, Request *out);

    // --- what the receipt says -------------------------------------------
    Outcome LastOutcome() const { return last_outcome_; }
    const char *LastTool() const { return last_tool_; }

    // Counters for a readout: how many were answered each way, and how many were
    // never answered at all. The third is the one worth watching (§10.10).
    uint16_t Allowed() const { return allowed_; }
    uint16_t Denied() const { return denied_; }
    uint16_t TimedOut() const { return timed_out_; }
    uint16_t Refused() const { return refused_; }

    // Presses thrown away by the guard above. Not an error — it is the guard
    // working — but a number that climbs means cards are arriving under fingers.
    uint16_t Ignored() const { return ignored_; }

   private:
    struct Slot {
        Request request;
        uint32_t deadline_ms;
    };

    void DropFront(uint32_t now_ms, Outcome outcome);
    void EnterReceipt(uint32_t now_ms, Outcome outcome, const char *tool);
    static bool Reached(uint32_t now_ms, uint32_t at_ms) {
        return static_cast<int32_t>(now_ms - at_ms) >= 0;
    }

    Slot queue_[kMaxPending] = {};
    uint8_t count_ = 0;

    CardState state_ = CardState::kIdle;

    // When the card now up was **presented**, which is not when it arrived: a
    // request that waited behind two others gets its own guard from the moment it
    // came up.
    uint32_t front_since_ms_ = 0;
    uint32_t receipt_until_ms_ = 0;

    Outcome last_outcome_ = Outcome::kNone;
    char last_tool_[kToolNameSize] = {};

    uint16_t allowed_ = 0;
    uint16_t denied_ = 0;
    uint16_t timed_out_ = 0;
    uint16_t refused_ = 0;
    uint16_t ignored_ = 0;
};

}  // namespace ui
