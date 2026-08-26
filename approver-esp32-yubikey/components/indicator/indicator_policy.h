#pragma once

// **What the device is, in one colour** (CLAUDE.md §10.17).
//
// The C6 board of the sibling folder answers "what is going on" with seven
// screens. This one has a single emitter, so the question has to be answered by
// *ranking*: at any moment several things are true — no internet, not
// registered, a request waiting — and exactly one of them gets the light. That
// ranking is the whole of this file, and it is the reason this is a component
// rather than four lines inside the responder.
//
// **The rank is by what the operator would fix first**, walking up from the
// bottom of the stack: a device with no Wi-Fi cannot have a bus, a device with
// no bus cannot be registered, and saying "not registered" to somebody whose
// router is off is a lie by omission. The one exception is at the top — a
// pending request outranks everything, including faults, because a request that
// is not shown is a request that times out (§10.10) and the operator can read
// the rest off the console afterwards.
//
// No ESP-IDF, no allocation, no clock: `Decide` is a function of its argument
// and `LookOf` is a table. §10.11's host tier runs both, which is what makes the
// ranking something you can test rather than something you have to watch a desk
// to believe.

#include <cstdint>

#include "led_frames.h"

namespace indicator {

// Everything the ranking needs, gathered by somebody else (§10.17: `main`
// registers the gatherer, and this component has never heard of a radio). Every
// field is a plain answer to a plain question, deliberately — a struct of
// booleans is a struct a test can enumerate.
struct Inputs {
    // The device has not finished `app_main` yet. Everything else is unknown
    // while this is true, and the light says exactly that rather than reporting
    // the absence of things that have not been brought up.
    bool booting = false;

    // The window in which holding BOOT restores `config.json` (§10.15). It is
    // brief, it is at boot, and it has its own colour because it is the one
    // moment on this device where a press does something irreversible.
    bool restore_window = false;

    // Something the operator has to know about that is not covered by a missing
    // link: a filesystem that would not mount, a key self-test that failed, an
    // LED write that keeps failing. The console is where the detail is.
    bool fault = false;

    bool storage_mounted = false;
    // **Whether the handler's signature can be checked** (§10.6). It used to be
    // `device_key` — whether this board had an Ed25519 identity — and since §10.18
    // there is no such key: what libsodium is still for is verifying §6's *reply*,
    // and a board that cannot do that must not register, because it cannot know
    // whose key it pinned. The light's meaning did not change when the name did.
    bool can_verify = false;
    bool wifi_link = false;    // associated with an access point
    bool internet = false;     // and the reachability check agrees (§10.9)
    bool bus_connected = false;
    bool registered = false;   // §10.7 — the handler knows this key_id
    bool subscribed = false;   // and the device is on `approvals.*`

    // The gate of §10.18. **There is no `fido_required` any more**: the key is not
    // a policy this device applies, it is where the private key lives, so "no key"
    // is always a device that cannot answer and the light always says so.
    bool fido_present = false;
    // **Whether this device has ever been introduced to a key** (§10.18), which
    // is a different fact from whether one is plugged in now: an enrolment is
    // permanent, presence is momentary. The responder refuses to subscribe
    // without the first and waits out the second.
    bool fido_enrolled = false;

    // A request is on the desk and nobody has answered it yet.
    bool request_pending = false;

    // The operator answered and the signature is being made. Brief — tens of
    // milliseconds — and it has a colour anyway, because the one failure mode
    // worth seeing on this device is a sign that never finishes.
    bool signing = false;

    // **A tap on BOOT chose `deny`, and the key has not signed it yet** (§10.18.5).
    // It needs a colour of its own because without one the light does not change at
    // all when the button is pressed: the operator taps, sees the same white flash,
    // and cannot tell whether the tap landed or whether touching the key now will
    // produce an allow. It did exactly that twice on the desk, both times ending in
    // an `allow` nobody meant.
    bool deny_pending = false;
};

// Ordered **worst first**, which is also the order `Decide` tests them in. The
// enumerator values are not a wire format and nothing persists them, so
// inserting a state in the middle is a source change and not a migration.
enum class State : uint8_t {
    kBooting = 0,
    kRestoreWindow,
    kSigning,
    kDenyPending,
    kPending,
    kFault,
    kNoStorage,
    kNoVerifier,
    kNoWifi,
    kNoInternet,
    kNoBus,
    kNotRegistered,
    kNotEnrolled,
    kNoFidoKey,
    kWatching,  // on the bus, reading, and unable to approve — a real state
    kReady,
};

const char *StateName(State state);

// One line of English for the console and for a log at boot. **Not a section
// number**: somebody reading this off a serial port at midnight wants to be told
// what to do, and §10.7's rule is that the console answers in words.
const char *StateText(State state);

struct Look {
    led::Rgb colour;
    led::Effect effect;
    // Whether this uses the operator's idle ceiling rather than the full one
    // (`config::Led`). Resting states do; anything that wants a human does not.
    bool idle;
};

// The ranking. Pure, total, and the only place that decides what beats what.
State Decide(const Inputs &in);

// The palette, as a table. Separated from `Decide` so that a test can assert
// "these two states must not look alike" without going through the ranking, and
// so that the ranking can be read without a colour in the way.
Look LookOf(State state);

}  // namespace indicator
