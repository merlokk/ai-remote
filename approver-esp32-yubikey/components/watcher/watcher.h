#pragma once

// **The two watch-only subscriptions** (CLAUDE.md §9.7, §9.10, §10.8.3) — the one
// thing on this device that listens and never answers.
//
// `status` is what a Claude Code session is *spending*; `activity` is what it is
// *doing*, off the hooks of §9.10. One connection, two subjects, one component:
// they are the same kind of thing — a broadcast current value from the same
// publisher — and splitting them would have meant two copies of the bookkeeping
// below with nothing different in either.
//
// It is a component of its own rather than three lines inside `responder` for the
// reason §10.8.3 states as a test: **deleting the limits screen must leave a
// working responder.** Two components, one dependency each way that does not
// exist, and that test is something you can perform rather than something to
// believe.
//
// The differences from the approval path are all one difference — this is a
// broadcast current value, not a request:
//
//   * **no queue group.** §10.5: a current value is meant to reach every
//     subscriber, and joining a group would take it from the other watchers;
//   * **it subscribes as soon as there is a socket.** No key and no registration
//     are needed to *read*, so a device that cannot approve anything still shows
//     what the session is spending;
//   * **junk keeps the last good document**, where a bad approval request is
//     dropped and forgotten. There is something worth keeping here;
//   * **nothing it does can reach a verdict.** It has no way to inject a request
//     and no way to sign one; **on this board it calls nothing at all** — it keeps
//     the last good document of each kind and hands them out to whoever asks.
//     That is the one structural difference from the sibling board's copy, where
//     the same two documents went to `screens::ShowLimits` / `ShowActivity`. With
//     no panel there is nowhere to push them, so the console pulls them instead,
//     and the component got smaller rather than larger for it.
//
// It has no task of its own. Deliveries arrive on the bus task, and the
// subscription is kept in step from the responder's tick — which is the one place
// the split above is a shade less than absolute, and it is a scheduling detail
// rather than a dependency: `Maintain()` here is what does the work, and the
// caller is only a clock.

#include <cstdint>

#include "activity_view.h"
#include "esp_err.h"
#include "limits_view.h"

namespace watcher {

// Why there are no numbers, in words rather than section numbers (§10.7).
enum class Blocker : uint8_t {
    kNone,
    kNoBus,
};

// What the two age fields below say when nothing has arrived yet.
inline constexpr uint32_t kNeverArrived = 0xFFFFFFFFu;

struct Status {
    bool ready = false;
    bool subscribed = false;
    // §9.10's subject, watched on the same connection and counted apart: a device
    // watching the numbers and not the activity is a state worth being able to see
    // rather than one to average away.
    bool activity_subscribed = false;
    Blocker blocked_by = Blocker::kNoBus;

    uint32_t received = 0;  // documents off the subject
    uint32_t refused = 0;   // …that would not parse, or were not one

    uint32_t activity_received = 0;
    uint32_t activity_refused = 0;

    // How long ago the last good document of each kind arrived, or
    // `kNeverArrived`. **An age rather than a timestamp**, because this device
    // has no RTC (§10.13) and a timestamp printed against a clock that has never
    // been set is a number nobody can use. `kUnknown` is what the sibling board
    // shows on its glass, and it is the same answer.
    uint32_t limits_age_ms = kNeverArrived;
    uint32_t activity_age_ms = kNeverArrived;
};


const char *BlockerText(Blocker blocker);

// Registers nothing and opens nothing: `Maintain` is what subscribes, once there
// is a connection to subscribe on.
esp_err_t Init();
bool Ready();

// Called on somebody else's tick. Subscribes when there is a socket, and notices
// when a reconnect has taken the subscription with it.
void Maintain();

Status Get();

// --- The last good document of each kind ----------------------------------
//
// **A copy, taken under the lock**, because the bus task can replace either of
// them between a caller's two reads and a struct half from one publish and half
// from the next is a readout nobody can trust. Both are small enough that the
// copy is cheaper than the alternative.
//
// `has_limits` / `has_activity` distinguish "nothing has ever arrived" from "a
// document arrived and every field in it was zero", which on a fresh session is
// the difference between a bus that is quiet and one that is not there.
ui::Limits Limits(bool *has_limits);
ui::Activity Activity(bool *has_activity);

}  // namespace watcher
