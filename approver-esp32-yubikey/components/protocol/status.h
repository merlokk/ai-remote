#pragma once

// **§9.7's `status` document, parsed** (CLAUDE.md §10.8.3).
//
// The other two files in this component are §6 and §7 — a protocol with a reply.
// This one has none: `status` is a *current value*, published by every render of
// the status line, never answered, and superseded a second later. A subscriber
// that was not listening missed it, by design (§4).
//
// Three consequences that shape this file rather than decorate it:
//
//   * **it is read-only, and the request path must not depend on it.** §10.8.3
//     states the test: deleting the limits screen must leave a working responder.
//     Nothing here is included by `approval.cpp` or `registration.cpp`, and the
//     bus subscription that feeds it is a component of its own;
//   * **absent is absent, never null.** §9.7 omits a whole section for an API key
//     rather than a subscription, so "no `rate_limits`" is an ordinary document
//     and not a malformed one. Every gauge carries its own `present`;
//   * **junk keeps the last good document.** A payload that will not parse, or
//     one missing `ts`, changes nothing on the screen — which is the opposite of
//     the approval path, where a bad message is dropped and forgotten. Here there
//     is something worth keeping.
//
// cJSON and `ui/limits_view.h`, which includes `<cstdint>` and the navigator. So
// this is host-tested like the rest of the component (§10.11).

#include <cstddef>

#include "limits_view.h"

namespace protocol {

// The subject (§9.7). **No queue group**, and that is not an omission: a
// broadcast current value is meant to reach every subscriber, and joining a group
// would mean taking it from the other watchers (§10.5).
inline constexpr char kStatusSubject[] = "status";

// Longer than any document §9.7 produces and short enough that a flood on an open
// subject costs nothing. Anything bigger is dropped unread (§10.10).
inline constexpr size_t kMaxStatusBytes = 2048;

// Fills `out` and returns true, or leaves it alone and returns false. The only
// hard requirements are that it is an object and that it carries a `ts` — every
// other field is optional, because §9.7 says a document with nothing but `ts` and
// `line` is a legitimate one.
bool ParseStatus(const char *json, size_t length, ui::Limits *out);

}  // namespace protocol
