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
//     and no way to sign one; the only things it calls are `screens::ShowLimits`
//     and `screens::ShowActivity`, neither of which can answer anything.
//
// It has no task of its own. Deliveries arrive on the bus task, and the
// subscription is kept in step from the responder's tick — which is the one place
// the split above is a shade less than absolute, and it is a scheduling detail
// rather than a dependency: `Maintain()` here is what does the work, and the
// caller is only a clock.

#include <cstdint>

#include "esp_err.h"

namespace watcher {

// Why there are no numbers, in words rather than section numbers (§10.7).
enum class Blocker : uint8_t {
    kNone,
    kNoBus,
};

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

}  // namespace watcher
