#pragma once

// **The loop, closed** (CLAUDE.md §7, §10.2, §10.8.4): take a request off
// `approvals.*`, put it in front of a human, sign what they pressed, and publish
// it into the subject the request arrived with.
//
// Everything under it already existed — a key (§10.6), a registration (§10.7),
// the bytes (§10.2), a card (§10.8.4) and a socket (§10.5). This is the ten
// lines of glue that make them one thing, and the interesting part is all in
// *when* rather than in *what*.
//
// ## The three rules it exists to keep
//
//   * **it subscribes only when it could actually answer.** §6's queue group
//     means each request reaches exactly one responder, so a device that
//     subscribed without a key, or without a registration, would take requests
//     away from the YubiKey responder and answer them with silence. Registered
//     plus a key plus a connection, or it is not on the subject at all — and
//     `request` on the console says which of the three is missing.
//   * **nothing is signed on the task that saw the press.** The screen task has
//     4 KB of stack in total and `crypto_sign` wants 4,112 bytes of it; more to
//     the point, a screen task that stalls cannot see the next press (§10.8.1).
//     A decision is copied into a slot and this component's own task does the
//     work.
//   * **a decision that missed its moment is dropped, not sent.** §10.10: if the
//     connection went and came back between the press and the publish, the inbox
//     the reply would go into no longer exists, and publishing into it is worse
//     than silence because it looks like an answer. The connection generation is
//     recorded at the press and checked at the publish.
//
// ## And the one it inherits
//
// **No reply is the safe outcome** (§10.10). Every failure here — a payload that
// will not parse, a queue that is full, a signature that fails, a socket that
// dropped — ends in nothing being published, the hook timing out, and Claude Code
// asking in its own terminal. There is no path that invents a verdict, and the
// only path to `allow` is a human pressing the button on the case.

#include <cstddef>
#include <cstdint>

#include "esp_err.h"

namespace responder {

// The task that signs. Sized from `crypto::kSignStackBytes` plus room for the
// JSON either side of it — the header holds the number because the console prints
// the low-water mark against it, and two copies of it would drift.
inline constexpr uint32_t kTaskStackBytes = 12288;

// Above the screen task, below LVGL's: a decision waiting to be signed is more
// urgent than the next frame of a clock face and less urgent than the frame the
// operator is looking at.
inline constexpr int kTaskPriority = 4;

// How often it looks at whether it should be subscribed. Slow, because the answer
// changes when a network does.
inline constexpr uint32_t kTickMs = 500;

// How many decided cards can be waiting to be signed. Two, and the number is an
// argument rather than a guess: a decision takes a human press and about ten
// milliseconds to sign, so one is almost always enough and the second is for the
// case where the queue of §10.8.4 brings the next card up instantly. Over
// capacity is a **drop**, which is §10.10's fail-safe and is counted.
inline constexpr size_t kPendingDecisions = 2;

// Why this device is not on the subject, in words rather than section numbers
// (§10.7). Ordered by what somebody would fix first.
enum class Blocker : uint8_t {
    kNone,
    kNoKey,
    kNotRegistered,
    kNoBus,
};

struct Status {
    bool ready = false;
    bool subscribed = false;
    Blocker blocked_by = Blocker::kNone;

    // Off the wire.
    uint32_t received = 0;  // deliveries on `approvals.*`
    uint32_t refused = 0;   // …that never became a card, for a stated reason
    uint32_t queued = 0;    // …that did

    // And back onto it.
    uint32_t replied = 0;
    uint32_t allowed = 0;
    uint32_t denied = 0;

    // Every one of these is a decision a human made that nobody heard.
    uint32_t sign_failed = 0;
    uint32_t publish_failed = 0;
    uint32_t stale_dropped = 0;  // the socket moved between the press and the reply
    uint32_t overflowed = 0;     // more decisions at once than there were slots

    uint32_t stack_low_water = 0;
};

const char *BlockerText(Blocker blocker);

// Starts the task and registers itself with `screens` as where a verdict goes.
// Subscribes to nothing yet: what happens next is whatever the key, the
// registration and the bus are doing.
esp_err_t Init();
bool Ready();

Status Get();

}  // namespace responder
