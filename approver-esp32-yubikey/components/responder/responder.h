#pragma once

// **The loop, closed** (CLAUDE.md §7, §10.2, §10.18): take a request off
// `approvals.*`, put it in front of a human, get a fingertip on a security key,
// and publish what that fingertip **signed** into the subject the request arrived
// with.
//
// Everything under it already exists — an enrolment (§10.18), a registration
// (§10.7), the bytes (§10.2), a queue (`ui::RequestCard`), a light (§10.17) and a
// socket (§10.5). This is the glue, and the interesting part is all in *when*
// rather than in *what*.
//
// ## Who decides, and who signs
//
// There is one button on this board and **no signing key on it at all** — the
// private half lives inside the security key (§10.18). So:
//
//   * **the button chooses, the key signs.** A tap on BOOT means `deny` and a
//     touch on the key means `allow`, but *either* verdict then has to be signed by
//     the key, because nothing else here can sign anything;
//   * **that makes a deny two gestures**, and §10.18.5 says so rather than hiding
//     it. Tapping and then walking away produces no reply, which is the third
//     outcome and the safe one;
//   * **a touch cannot be replayed onto a different request**, because what the
//     key signs is that request's own bytes — the verdict included, so an `allow`
//     touch cannot be turned into a `deny` or the other way round.
//
// ## The four rules it exists to keep
//
//   * **it subscribes only when it could actually answer.** §6's queue group
//     means each request reaches exactly one responder, so a device that
//     subscribed without an enrolment, or with a registration naming a key it no
//     longer holds, would take requests away from the YubiKey responder and answer
//     them with silence. Enrolled, registered **for that key**, and connected, or
//     it is not on the subject at all — and `request` on the console says which is
//     missing;
//   * **nothing is published on the task that ran the gate.** The gate blocks for
//     up to half a minute waiting for a finger; a task that both waited and
//     published would be a task that cannot notice a reconnect. A decision **and
//     the signature the key made for it** are copied into a slot and this
//     component's *other* task does the work;
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
// does not verify, a socket that dropped — ends in nothing being published, the
// hook timing out, and Claude Code asking in its own terminal. There is no path
// that invents a verdict, and there is no path to *any* verdict that does not go
// through a human touching a key.

#include <cstddef>
#include <cstdint>

#include "esp_err.h"

namespace responder {

// The task that publishes. It no longer signs — the signature arrives from the
// key — but the size is kept: it holds a 2.3 KB request, the reply JSON and the
// base64 around it, and the console prints its low-water mark against this number.
inline constexpr uint32_t kTaskStackBytes = 12288;

// The task that waits for a fingertip. It holds a CTAP request and response buffer
// indirectly (they are static in `fido.cpp`), assembles §7's bytes, and runs **two**
// ECDSA P-256 verifications per approval — the assertion's and the verdict's — which
// is the deepest stack in this firmware.
//
// **12 KB because 8 KB was measured, not guessed at, and it was thin.** The first
// real end-to-end approval left `1280` bytes of 8192 free — 15 %, on the one path
// in this firmware that must not fail, where a crash is a request nobody answers.
// At 12288 the same path leaves `5360`, so the peak is **6,928 bytes** and the two
// measurements agree to within 16: this is a real number and not a fluctuation.
//
// The figure is a true high-water mark — `uxTaskGetStackHighWaterMark` is sampled
// after the gate returns, so it covers the publish too — and the 4 KB comes out of
// the ~187 KB of internal RAM this board has spare. **Internal, not the PSRAM**: a
// task stack is the one thing that must not live there (§10.13), whatever
// `CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM` allows. `request` prints the
// low-water mark against this number, which is how the 1280 was found.
inline constexpr uint32_t kGateStackBytes = 12288;

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

// Why this device is not on the subject, in words rather than section numbers
// (§10.7). Ordered by what somebody would fix first.
enum class Blocker : uint8_t {
    kNone,
    kCannotVerify,
    kNotRegistered,
    kNoBus,
    // **No key enrolled** (§10.18) — a device that could never say `allow`, or
    // `deny` either, whatever anybody does.
    //
    // Since the verdict is signed inside the security key, an enrolment is not a
    // policy this device applies; it is the private key existing at all. There is
    // no setting that relaxes it and there cannot be one.
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

// **A tap on BOOT chose `deny` and the key has not signed it yet** (§10.18.5) — the
// window between the two things a deny costs. The light has a colour for it
// (§10.17) because without one nothing changes when the button is pressed, and an
// operator who cannot see that the tap landed touches the key and gets an `allow`.
bool DenyPending();

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
