#pragma once

// The runtime half of §10.17: ask somebody what the device is, rank it, and put
// the answer on the emitter.
//
// **This component knows about a light and about a struct of booleans, and about
// nothing else.** It has never heard of a radio, a bus, a key or a request — the
// facts arrive through a gatherer `main` registers, which is the same hook shape
// `config::OnChanged` uses and for the same reason: the list of who to ask would
// otherwise be duplicated in every caller that wants the light refreshed.
//
// The test that this layering is real: **deleting `responder` must leave a
// working indicator**, and deleting this must leave a working responder. Neither
// includes the other.
//
// The ranking itself, and the palette, are `indicator_policy.h` — pure, and run
// by §10.11's host tier.

#include <cstdint>

#include "esp_err.h"
#include "indicator_policy.h"

namespace indicator {

// Small — it fills a struct, compares two enums and calls into `led`.
inline constexpr uint32_t kTaskStackBytes = 3072;

// Below `led`'s own task and far below the responder's. Being one tick late to
// notice a state change costs nothing; being one tick late to sign costs a
// verdict.
inline constexpr int kTaskPriority = 2;

// How often the gatherer is called. Half a second: a state change on this device
// is a network event or a human, and neither is faster than that. A request does
// not wait for it — `Announce` is pushed the moment it arrives.
inline constexpr uint32_t kTickMs = 500;

// How long a verdict flashes before the light goes back to whatever the ranking
// says. Long enough to be seen by somebody who has just taken their hand off the
// key, short enough that the device is visibly ready again before they look away.
inline constexpr uint32_t kVerdictFlashMs = 1500;

// Fills `out` with what the device currently is. Registered by `main`, which is
// the one place in this firmware that is allowed to know every subsystem.
using Gatherer = void (*)(Inputs *out);

// Starts the task. `led::Init` must have run; a gatherer may be registered
// before or after, and until one is the light shows `kBooting`.
esp_err_t Init();
bool Ready();

void OnGather(Gatherer gatherer);

// Re-gather now rather than at the next tick. For the one caller with a deadline
// — a request arriving — and for the console, so that a `led` readout is never
// stale by half a second.
void Poke();

// **Pushed rather than polled** (§10.17): a verdict is an event, not a state, and
// by the time the next tick came round the device would already be idle again with
// nothing to show. Green for an allow, red for a deny, both at full brightness for
// `kVerdictFlashMs`.
void ShowVerdict(bool allowed);

// **The touch prompt**, and the second thing that is pushed: blue and fast, while
// a *console* command is waiting for a fingertip on the security key.
//
// A real request needs none of this — the ranking already shows `pending`, white
// and fast, and that light means "touch the key" (§10.17). What had no light at all
// was `key enrol` and `key test`, which ask for a touch from a console the operator
// may not be looking at, on a device whose only output was still reporting whatever
// it happens to be. Blue because `signing` is blue too, so the pair reads in order:
// **flashing** is "I need you", **solid** is "got it".
//
// `duration_ms` should be the wait the caller is about to make; `EndTouchPrompt` is
// what the caller owes it afterwards, because a light still asking for a fingertip
// after one arrived is a light that lies.
void ShowTouchPrompt(uint32_t duration_ms);
void EndTouchPrompt();

// What the ranking last decided, for `status` and for one log line when it
// changes.
State Current();

// How many times the state has changed since boot. A number that climbs while
// nobody is touching the device is a network that is flapping, and it is the
// cheapest way to see that from a console.
uint32_t Transitions();

}  // namespace indicator
