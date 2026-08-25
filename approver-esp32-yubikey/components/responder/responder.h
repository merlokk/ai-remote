#pragma once

// **The loop, closed** (CLAUDE.md §7, §10.2, §10.18): take a request off
// `approvals.*`, put it in front of a human, get a fingertip on a security key,
// sign what that fingertip authorised, and publish it into the subject the
// request arrived with.
//
// Everything under it already exists — a key (§10.6), a registration (§10.7), the
// bytes (§10.2), a queue (`ui::RequestCard`), a light (§10.17), a gate (§10.18)
// and a socket (§10.5). This is the glue, and the interesting part is all in
// *when* rather than in *what*.
//
// ## How this differs from the C6 board's responder, in one paragraph
//
// There, a request went on a screen and a press on the case decided it. Here
// there is no screen and one button, so the two halves of a decision are split
// by **which one is safe to make cheap**:
//
//   * **`deny` is a button.** One short press, no key, no ceremony. Refusing
//     something is never the dangerous direction (§10.10);
//   * **`allow` is a key.** The device asks a FIDO authenticator for an assertion
//     over *the exact bytes it is about to sign*, and the key will not answer
//     until somebody touches it. No touch, no allow — and the touch cannot be
//     replayed onto a different request, because a different request is different
//     bytes.
//
// ## The four rules it exists to keep
//
//   * **it subscribes only when it could actually answer.** §6's queue group
//     means each request reaches exactly one responder, so a device that
//     subscribed without a key, or without a registration, would take requests
//     away from the YubiKey responder and answer them with silence. Registered
//     plus a key plus a connection, or it is not on the subject at all — and
//     `request` on the console says which of the three is missing;
//   * **nothing is signed on the task that ran the gate.** The gate blocks for
//     up to half a minute waiting for a finger; a task that both waited and
//     signed would be a task that cannot notice a reconnect. A decision is copied
//     into a slot and this component's *other* task does the work;
//   * **a decision that missed its moment is dropped, not sent.** §10.10: if the
//     connection went and came back between the touch and the publish, the inbox
//     the reply would go into no longer exists, and publishing into it is worse
//     than silence because it looks like an answer. The connection generation is
//     recorded at the decision and checked at the publish;
//   * **the gate's answer is never inferred.** A gate that timed out, was
//     cancelled, or returned something that did not verify produces **no reply**
//     — not a deny. A deny is a statement somebody made; silence is what a device
//     says when nobody made one.
//
// ## And the one it inherits
//
// **No reply is the safe outcome** (§10.10). Every failure here — a payload that
// will not parse, a queue that is full, a key that is not there, a signature that
// fails, a socket that dropped — ends in nothing being published, the hook timing
// out, and Claude Code asking in its own terminal. There is no path that invents
// a verdict, and the only path to `allow` is a human touching a key.

#include <cstddef>
#include <cstdint>

#include "esp_err.h"

namespace responder {

// The task that signs. Sized from `crypto::kSignStackBytes` plus room for the
// JSON either side of it — the header holds the number because the console prints
// the low-water mark against it, and two copies of it would drift.
inline constexpr uint32_t kTaskStackBytes = 12288;

// The task that waits for a fingertip. It holds a CTAP request and response
// buffer indirectly (they are static in `fido.cpp`) but it does run an ECDSA
// P-256 verification, which is the deepest stack in this firmware after the
// Ed25519 signature above.
inline constexpr uint32_t kGateStackBytes = 8192;

// Above the LED and the indicator, below the USB client. A decision waiting to be
// signed is more urgent than the next frame of a breath and less urgent than the
// completion callback that says the key answered.
inline constexpr int kTaskPriority = 4;
inline constexpr int kGatePriority = 4;

// How often the responder task looks at whether it should be subscribed. Slow,
// because the answer changes when a network does.
inline constexpr uint32_t kTickMs = 500;

// How often the gate task looks for something to do when there is nothing.
inline constexpr uint32_t kGateIdleMs = 100;

// How many decided requests can be waiting to be signed. Two, and the number is
// an argument rather than a guess: a decision takes a human touch and about ten
// milliseconds to sign, so one is almost always enough and the second is for the
// case where the queue brings the next request up instantly. Over capacity is a
// **drop**, which is §10.10's fail-safe and is counted.
inline constexpr size_t kPendingDecisions = 2;

// In the development mode of §10.18 (`approval.requireKey` false), how long BOOT
// has to be held to mean `allow`. **Deliberately awkward**: two seconds is long
// enough that nobody produces one by accident, and the awkwardness is the honest
// signal that this mode is not the one the device is meant to run in.
inline constexpr uint32_t kButtonAllowHoldMs = 2000;

// Why this device is not on the subject, in words rather than section numbers
// (§10.7). Ordered by what somebody would fix first.
enum class Blocker : uint8_t {
    kNone,
    kNoKey,
    kNotRegistered,
    kNoBus,
    // **No key enrolled, with the gate switched on** (§10.18) — a device that
    // could never say `allow` whatever anybody does.
    //
    // This one is not on the sibling board's list and it is the most important
    // entry here, because it was found the hard way: the device registered, went
    // straight onto `approvals.*` with nothing enrolled, and started taking
    // requests out of the `approvers` queue group that it could not answer. §6's
    // queue group means each request reaches exactly *one* responder, so every
    // request this device swallowed was a request the YubiKey responder never
    // saw — answered with silence, and looking from the outside exactly like a
    // bus that had gone quiet.
    //
    // **Enrolment blocks; a key not being plugged in does not.** The two are
    // different kinds of fact: an enrolment is permanent and its absence means
    // *never*, while a key in a pocket is a fifteen-second problem the gate
    // already waits out (`WaitForKey`). Blocking on presence would take this
    // device off the subject every time the operator walked away with the key.
    kNotEnrolled,
};

struct Status {
    bool ready = false;
    bool subscribed = false;
    Blocker blocked_by = Blocker::kNone;

    // Off the wire.
    uint32_t received = 0;  // deliveries on `approvals.*`
    uint32_t refused = 0;   // …that never became a pending request, for a stated reason
    uint32_t queued = 0;    // …that did

    // What the gate said. `gate_declined` is every non-approval that was not a
    // button deny — a timeout, an unplugged key, an assertion that did not
    // verify. None of them produced a reply.
    uint32_t gate_asked = 0;
    uint32_t gate_approved = 0;
    uint32_t gate_declined = 0;
    uint32_t button_denied = 0;

    // And back onto it.
    uint32_t replied = 0;
    uint32_t allowed = 0;
    uint32_t denied = 0;

    // Every one of these is a decision a human made that nobody heard.
    uint32_t sign_failed = 0;
    uint32_t publish_failed = 0;
    uint32_t stale_dropped = 0;  // the socket moved between the touch and the reply
    uint32_t overflowed = 0;     // more decisions at once than there were slots

    uint32_t stack_low_water = 0;
    uint32_t gate_stack_low_water = 0;
};

const char *BlockerText(Blocker blocker);

// Starts both tasks. Subscribes to nothing yet: what happens next is whatever the
// key, the registration and the bus are doing.
esp_err_t Init();
bool Ready();

Status Get();

// --- What the light needs to know (§10.17) --------------------------------
//
// Three questions `main`'s gatherer asks every tick. They are here rather than in
// `Status` because they are read twice a second and `Status` is a 100-byte copy
// under a mutex.

// A request is up and nobody has answered it.
bool RequestPending();

// **A decision is being signed right now.** Tens of milliseconds in the ordinary
// case, and it has a colour anyway (§10.17) because the one failure worth seeing
// on this device is a signature that never finishes.
//
// **Not "the gate is running"**, which is what this was first and which was wrong
// on the board: the gate runs for the whole time a request is on the desk, so the
// light showed `signing` — blue, solid — for the entire thirty seconds it should
// have been showing `pending`. Found by typing `request test` and looking at the
// LED, which is the only way that class of bug is ever found.
bool Busy();

// What is on the desk right now, for `request` on the console. Null when nothing
// is pending. **Valid only until the next tick** — the caller prints it and lets
// it go.
struct PendingView {
    bool present = false;
    char tool_name[32] = {};
    char cwd[160] = {};
    uint32_t remaining_ms = 0;
    uint8_t waiting = 0;
};

PendingView Front();

// **Inject a request as if it had come off the bus.** For `request test` on the
// console (§10.7), which is how the light, the queue and the gate are exercised
// with no NATS server and no hook.
//
// It goes through exactly the same path a real one does, with one difference
// that is stated where it is used: its reply subject is `kTestReplySubject`, and
// a decision carrying that subject is decided, counted, flashed on the LED and
// **never published**.
//
// **The sentinel is a subject rather than an empty string**, and that is not a
// detail. `ui::RequestCard::Arrived` refuses a request with no reply-to — §10.10,
// a request nobody could answer is not a request — so a test that used an empty
// one would be refused at the door and would exercise nothing. A recognisable
// subject goes through the whole path and is stopped at the last step, which is
// the only step it must not take.
inline constexpr const char *kTestReplySubject = "console.test.no-reply";

bool InjectTestRequest(const char *tool_name, const char *text, uint32_t ttl_ms);

}  // namespace responder
