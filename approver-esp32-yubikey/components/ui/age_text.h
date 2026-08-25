#pragma once

// **How long ago, in the units a person reads**: `3 s`, `66 m`, `8h 05m`.
//
// This used to live in `limits_view.h`, next to the screen that put an age under
// §9.7's numbers. That screen is gone with the panel and so is the watcher that
// fed it — this device shows nothing and watches nothing — but the console still
// prints ages: how long since the clock last synced, how long the bus has been
// up, how long a request has left. So the function outlived its screen and this
// is where it landed.
//
// It stays in `ui` rather than moving into `components/cli` for the reason that
// component's CMakeLists gives about the namespace: `ui` is the name §7's
// `ui::Request` is stuck with, the console already reaches in here for it, and
// this file is the same kind of thing — arithmetic with no hardware under it, so
// §10.11's host tier compiles it with a bare C++ compiler and pins the bands.
// `cli/console.cpp` calls this rather than keeping its own copy, which is the one
// property worth protecting: one implementation of "how long ago", so a paste of
// `date` and a paste of `nats` cannot describe the same instant two ways.
//
// **This file includes `<cstddef>` and `<cstdint>` and nothing else.**

#include <cstddef>
#include <cstdint>

namespace ui {

// Room for `1193046h 12m`, which is what a `uint32_t` of seconds can reach, plus
// the terminator.
inline constexpr size_t kAgeTextSize = 16;

// Writes into the caller's buffer, never allocates, always terminates. `4000 s`
// is the reading this replaces — "21600 s ago is a conversion nobody should have
// to do in their head".
void AgeText(uint32_t seconds, char *out, size_t capacity);

}  // namespace ui
