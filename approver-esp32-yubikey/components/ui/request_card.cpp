#include "request_card.h"

namespace ui {

uint32_t EffectiveTtlMs(const Request &request) {
    return request.ttl_ms != 0 ? request.ttl_ms : RequestCard::kDefaultTtlMs;
}
namespace {

// `strnlen` without `<cstring>`, so this file keeps the include list its header
// advertises. Six lines against a dependency that would put the whole of the C
// string library into the one object the host tests care most about.
size_t Length(const char *text, size_t capacity) {
    size_t length = 0;
    while (length < capacity && text[length] != '\0') {
        ++length;
    }
    return length;
}

// True when the field is text that terminated inside its buffer. A field whose
// last byte is not a NUL either overflowed or arrived without one, and both mean
// the same thing here: it is not a string this device is going to show somebody
// and ask them to approve.
bool Terminated(const char *text, size_t capacity) {
    return Length(text, capacity) < capacity;
}

void Copy(char *destination, const char *source, size_t capacity) {
    size_t i = 0;
    for (; i + 1 < capacity && source[i] != '\0'; ++i) {
        destination[i] = source[i];
    }
    destination[i] = '\0';
}

}  // namespace

bool RequestCard::Arrived(const Request &request, uint32_t now_ms) {
    if (count_ >= kMaxPending) {
        // §10.14.1: full is a state that was designed. §10.10 says what the
        // caller does with a `false` — drop it, one log line, no reply.
        ++refused_;
        return false;
    }

    // **Every text field, checked in one place.** The parser could refuse an
    // over-long `tool_input` on its way in and this would still have to know,
    // because a `Request` can be built by anything — the console's test card, a
    // future bus handler, a test. One rule, at the door.
    const bool fits = Terminated(request.session_id, kSessionIdSize) &&
                      Terminated(request.nonce, kNonceSize) &&
                      Terminated(request.input_sha256, kSha256HexSize) &&
                      Terminated(request.tool_name, kToolNameSize) &&
                      Terminated(request.cwd, kCwdSize) &&
                      Terminated(request.tool_input, kToolInputSize) &&
                      Terminated(request.reply, kReplySubjectSize);
    if (!fits) {
        ++refused_;
        return false;
    }

    // A card nobody could answer is not a card (§10.10: never publish into a
    // dead inbox — and an empty subject is the deadest of them).
    if (request.reply[0] == '\0') {
        ++refused_;
        return false;
    }

    // And a tool with no name is not something to ask a human about: it is the
    // one field the whole card is a question about.
    if (request.tool_name[0] == '\0') {
        ++refused_;
        return false;
    }

    Slot &slot = queue_[count_];
    slot.request = request;
    slot.deadline_ms = now_ms + EffectiveTtlMs(request);
    ++count_;

    if (count_ == 1) {
        // It is the card now, so its guard starts here — and it **replaces a
        // receipt**, because §10.8.1's rule that nothing steals focus has one
        // exception and this is it: a pending request outranks a note about a
        // finished one.
        state_ = CardState::kCard;
        front_since_ms_ = now_ms;
    }
    return true;
}

const Request *RequestCard::Front() const {
    if (state_ != CardState::kCard || count_ == 0) {
        return nullptr;
    }
    return &queue_[0].request;
}

uint32_t RequestCard::RemainingMs(uint32_t now_ms) const {
    if (state_ != CardState::kCard || count_ == 0) {
        return 0;
    }
    const int32_t left = static_cast<int32_t>(queue_[0].deadline_ms - now_ms);
    return left > 0 ? static_cast<uint32_t>(left) : 0;
}

void RequestCard::EnterReceipt(uint32_t now_ms, Outcome outcome, const char *tool) {
    last_outcome_ = outcome;
    Copy(last_tool_, tool, kToolNameSize);
    state_ = CardState::kReceipt;
    receipt_until_ms_ = now_ms + kReceiptMs;
}

void RequestCard::DropFront(uint32_t now_ms, Outcome outcome) {
    if (count_ == 0) {
        return;
    }

    // The tool name is read before the shift, because after it the slot belongs
    // to whatever was behind.
    char tool[kToolNameSize];
    Copy(tool, queue_[0].request.tool_name, kToolNameSize);

    for (uint8_t i = 1; i < count_; ++i) {
        queue_[i - 1] = queue_[i];
    }
    --count_;

    if (count_ > 0) {
        // §10.8.4: "Answering one brings the next up instantly — which is
        // precisely when the 300 ms guard earns its place." So the next card is
        // presented now and its guard starts now, and **there is no receipt**: a
        // note about the last request must not sit in front of the next one.
        state_ = CardState::kCard;
        front_since_ms_ = now_ms;
        last_outcome_ = outcome;
        Copy(last_tool_, tool, kToolNameSize);
    } else {
        EnterReceipt(now_ms, outcome, tool);
    }
}

bool RequestCard::Tick(uint32_t now_ms) {
    bool changed = false;

    // **The queue is aged from the back as well as the front.** A request that
    // sat behind two others past its own TTL is a request the hook stopped
    // waiting for, and bringing it up would put a card on screen whose press
    // could only publish into a dead inbox.
    uint8_t i = count_;
    while (i > 1) {
        --i;
        if (Reached(now_ms, queue_[i].deadline_ms)) {
            for (uint8_t j = static_cast<uint8_t>(i + 1); j < count_; ++j) {
                queue_[j - 1] = queue_[j];
            }
            --count_;
            ++timed_out_;
            changed = true;
        }
    }

    if (state_ == CardState::kCard && count_ > 0 && Reached(now_ms, queue_[0].deadline_ms)) {
        ++timed_out_;
        DropFront(now_ms, Outcome::kTimedOut);
        changed = true;
    }

    if (state_ == CardState::kReceipt && Reached(now_ms, receipt_until_ms_)) {
        state_ = CardState::kIdle;
        changed = true;
    }

    return changed;
}

bool RequestCard::Press(Verdict verdict, uint32_t pressed_at_ms, Request *out) {
    if (state_ != CardState::kCard || count_ == 0) {
        return false;
    }

    // **One comparison, both halves of §10.8.1's rule.** A finger that went down
    // before the card appeared gives a negative difference; a card that has been
    // up for less than the guard gives a small one. Signed, so the ~49-day wrap
    // is arithmetic rather than a case.
    if (static_cast<int32_t>(pressed_at_ms - front_since_ms_) <
        static_cast<int32_t>(kPressGuardMs)) {
        ++ignored_;
        return false;
    }

    if (out != nullptr) {
        *out = queue_[0].request;
    }

    if (verdict == Verdict::kAllow) {
        ++allowed_;
    } else {
        ++denied_;
    }
    DropFront(pressed_at_ms, verdict == Verdict::kAllow ? Outcome::kAllowed : Outcome::kDenied);
    return true;
}

}  // namespace ui
