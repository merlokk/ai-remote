#pragma once

// **§9.10's `activity` document, parsed** (CLAUDE.md §10.8.3).
//
// `status.cpp` next door reads what a session is *spending*; this reads what it is
// *doing*, off the subject the same binary publishes from Claude Code's
// `PreToolUse` / `PostToolUse` / `Stop` hooks. Everything that file says about its
// own document holds here — read-only, no reply, absent is absent, junk keeps the
// last good one — with one addition of its own:
//
//   * **`v` is required and pinned.** §9.7's document is recognisable by always
//     carrying `ts` *and* `line`; this one has no such pair, because every field
//     but three may be absent. So `v == 1` is both the version check and the
//     "this is ours" test on an open subject, and a `v: 2` from a newer publisher
//     is refused rather than half-understood.
//
// The other difference is what is *not* here: `event` and `state` are enums by the
// time they leave this file. A name this firmware does not know is a refusal, so
// nothing downstream has to decide what to draw for a word it has never seen.

#include <cstddef>

#include "activity_view.h"

namespace protocol {

// The subject (§9.10). **No queue group**, for the reason `kStatusSubject` gives:
// a broadcast current value is meant to reach every subscriber.
inline constexpr char kActivitySubject[] = "activity";

// The schema version this firmware understands (§9.10).
inline constexpr int kActivityVersion = 1;

// Half of `kMaxStatusBytes`, and that is a measurement rather than a round
// number: the longest document §9.10 produces is a `pre_tool` with an 80-character
// summary, a 48-character tool name and every optional field set, which is under
// 400 bytes. Anything bigger is dropped unread (§10.10).
inline constexpr size_t kMaxActivityBytes = 1024;

// Fills `out` and returns true, or leaves it alone and returns false. Required:
// an object, `v == kActivityVersion`, a `ts`, and an `event` and `state` this
// firmware knows. Everything else is optional.
bool ParseActivity(const char *json, size_t length, ui::Activity *out);

}  // namespace protocol
