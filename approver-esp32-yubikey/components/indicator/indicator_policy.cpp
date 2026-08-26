// The ranking and the palette (CLAUDE.md §10.17). Pure — §10.11's host tier runs
// every line of this file with no board and no ESP-IDF.

#include "indicator_policy.h"

namespace indicator {

State Decide(const Inputs &in) {
    // **Boot first, because nothing else is known yet.** Reporting "no bus" for
    // the two seconds before the bus has been asked to connect would be true and
    // useless.
    if (in.booting) {
        return State::kBooting;
    }
    if (in.restore_window) {
        return State::kRestoreWindow;
    }

    // **The top of the ranking, and the one place the "fix it lowest first" rule
    // is deliberately broken.** A request has a deadline; everything below has
    // none. A device that showed a fault instead of a waiting request would be a
    // device that let the request expire in order to report something the
    // console could have said at any time.
    if (in.signing) {
        return State::kSigning;
    }
    // **Above `pending`, because the request is no longer the news.** A tap on BOOT
    // has chosen a verdict and what is owed now is a touch to sign it (§10.18.5).
    // Saying "a request is waiting" at that point is saying the thing the operator
    // has already done something about.
    if (in.deny_pending) {
        return State::kDenyPending;
    }
    if (in.request_pending) {
        return State::kPending;
    }

    if (in.fault) {
        return State::kFault;
    }

    // From here down it is the stack, bottom up: each of these makes the ones
    // after it impossible, so the first one that is false is the one to fix.
    if (!in.storage_mounted) {
        return State::kNoStorage;
    }
    if (!in.can_verify) {
        return State::kNoVerifier;
    }
    if (!in.wifi_link) {
        return State::kNoWifi;
    }
    // **Associated is not online** (§10.9), and the two have separate colours
    // because they have separate fixes: one is a passphrase, the other is
    // somebody else's router.
    if (!in.internet) {
        return State::kNoInternet;
    }
    if (!in.bus_connected) {
        return State::kNoBus;
    }
    // **Enrolment before registration, and this is the bottom-up rule rather
    // than an exception to it.** `register` refuses without an enrolment,
    // because what §6 registers is the key derived from it (§10.18.1): so an
    // unenrolled device that reported `not-registered` would be sending an
    // operator to mint a one-time token they cannot spend yet, and the token
    // would be spent by the time they found out. It is also the state that
    // keeps this device off `approvals.*` entirely.
    if (!in.fido_enrolled) {
        return State::kNotEnrolled;
    }
    if (!in.registered) {
        return State::kNotRegistered;
    }
    if (!in.fido_present) {
        return State::kNoFidoKey;
    }
    // Registered, connected, keyed — and still not on `approvals.*`. Rare and
    // brief (the responder subscribes on its next tick), but it is a state the
    // device can genuinely be in, and calling it "ready" would be a light that
    // promises an answer nobody is listening to give.
    if (!in.subscribed) {
        return State::kWatching;
    }
    return State::kReady;
}

const char *StateName(State state) {
    switch (state) {
        case State::kBooting:
            return "booting";
        case State::kRestoreWindow:
            return "restore-window";
        case State::kSigning:
            return "signing";
        case State::kDenyPending:
            return "deny-pending";
        case State::kPending:
            return "pending";
        case State::kFault:
            return "fault";
        case State::kNoStorage:
            return "no-storage";
        case State::kNoVerifier:
            return "no-verifier";
        case State::kNoWifi:
            return "no-wifi";
        case State::kNoInternet:
            return "no-internet";
        case State::kNoBus:
            return "no-bus";
        case State::kNotRegistered:
            return "not-registered";
        case State::kNotEnrolled:
            return "not-enrolled";
        case State::kNoFidoKey:
            return "no-fido-key";
        case State::kWatching:
            return "watching";
        case State::kReady:
            return "ready";
    }
    return "?";
}

const char *StateText(State state) {
    switch (state) {
        case State::kBooting:
            return "starting up";
        case State::kRestoreWindow:
            return "hold BOOT to restore config.json";
        case State::kSigning:
            return "signing the decision";
        case State::kDenyPending:
            return "deny chosen - touch the key to sign it";
        case State::kPending:
            return "a request is waiting for you";
        case State::kFault:
            return "something failed - see `status` on the console";
        case State::kNoStorage:
            return "the storage partition would not mount";
        case State::kNoVerifier:
            return "no device key - see `keys`";
        case State::kNoWifi:
            return "no Wi-Fi link - see `wifi`";
        case State::kNoInternet:
            return "associated, but nothing answers - see `wifi check`";
        case State::kNoBus:
            return "no NATS connection - see `nats`";
        case State::kNotRegistered:
            return "not registered - run `register <token>`";
        case State::kNotEnrolled:
            return "no security key enrolled - run `key enrol`";
        case State::kNoFidoKey:
            return "plug a security key into the OTG port";
        case State::kWatching:
            return "connected, not yet on approvals.*";
        case State::kReady:
            return "ready";
    }
    return "?";
}

Look LookOf(State state) {
    switch (state) {
        // **Red, solid, the moment there is power** (§10.17). The repository
        // owner asked for this one by name, and it earns the place: red-on-boot
        // is the only state that proves the emitter, the UART and the encoding
        // are all working, because it appears before anything else has had a
        // chance to go wrong. A board that comes up dark has a hardware fault; a
        // board that comes up red has a firmware that is running.
        case State::kBooting:
            return {led::colour::kRed, led::Effect::kSolid, false};

        // White, solid, at full: the restore window is the one moment where a
        // press is irreversible, and it lasts seconds. It gets the brightest
        // thing this device can do, and white is not used anywhere else on the
        // way up.
        case State::kRestoreWindow:
            return {led::colour::kWhite, led::Effect::kSolid, false};

        // **White, fast, full brightness — the only state whose job is to be
        // noticed from across a room** (§10.17). Everything else here is a
        // readout; this one is a request with a deadline on it.
        //
        // **White rather than amber, and that is the yellow above talking.**
        // Every state between power-on and a connected bus is yellow now, and
        // amber-versus-yellow on a bare WS2812 is a distinction that survives a
        // diagram and not a desk. The one state that must never be misread got
        // the one colour nothing else uses.
        case State::kPending:
            return {led::colour::kWhite, led::Effect::kFastBlink, false};

        // Blue, solid. Tens of milliseconds in the ordinary case, so what this
        // colour really reports is a signature that got *stuck*.
        case State::kSigning:
            return {led::colour::kBlue, led::Effect::kSolid, false};

        // **Red, and at the middle rate on purpose.** Red is what a deny is on this
        // device — the verdict flash is red — so the light that says "your deny was
        // heard, now sign it" is red too, and the operator sees the same colour
        // before and after the touch.
        //
        // Not *fast*, which is `fault`: red-fast already means something, and the
        // rule that no two states may look alike is a test
        // (`test_indicator_no_two_states_look_alike_outside_the_yellow_stack`) and
        // not a preference. The rate is also honest about the difference — a fault
        // is the device's problem and this is a job still owed to it.
        case State::kDenyPending:
            return {led::colour::kRed, led::Effect::kNormBlink, false};

        // Red, fast — and it is red twice on this device, once solid at boot and
        // once blinking here. The pairing is the point: red is "this device",
        // and the rhythm says whether it is starting or stopping.
        case State::kFault:
            return {led::colour::kRed, led::Effect::kFastBlink, false};

        // --- Everything between power-on and a bus (§10.17) -----------------
        //
        // **One colour and one rhythm for all five, which is the repository
        // owner's instruction and is also the honest design.** Storage, key,
        // Wi-Fi, internet and bus are a stack: each makes the next possible, the
        // operator has one thing to do about all of them — plug the device into a
        // network that has the bus on it — and five yellows told apart by blink
        // rate would be five things to memorise for one action.
        //
        // What is lost is diagnosis at a glance, and it is not lost far: `status`
        // on the console names the exact rung, `indicator::StateText` puts it in
        // words, and every transition between them is a log line. The light says
        // *not yet*; the console says *why*.
        case State::kNoStorage:
        case State::kNoVerifier:
        case State::kNoWifi:
        case State::kNoInternet:
        case State::kNoBus:
            return {led::colour::kYellow, led::Effect::kFastBlink, false};

        // --- Past the bus, and now the differences matter -------------------
        //
        // These three are the states a *connected* device can be stuck in, and
        // each has a different thing for the operator to do — mint a token, plug
        // in a key, or wait a tick. That is what earns them colours of their own
        // where the five above share one.

        // Registration is a missing piece with a command that fixes it — and the
        // *second* one a new device asks for, because an enrolment has to come
        // first (§10.18.1) and outranks it above.
        case State::kNotRegistered:
            return {led::colour::kMagenta, led::Effect::kNormBlink, false};

        // **Cyan, fast, at full brightness — the one cyan that is asking for
        // something.** A device with nothing enrolled is not on `approvals.*` at
        // all (`responder::Blocker::kNotEnrolled`) and cannot even be registered,
        // so this is as stuck as a device gets without anything being broken —
        // and it is one console command away from being fixed. It is also what a
        // brand-new device shows first.
        case State::kNotEnrolled:
            return {led::colour::kCyan, led::Effect::kFastBlink, false};

        // Cyan: everything works except the thing in your pocket (§10.18).
        case State::kNoFidoKey:
            return {led::colour::kCyan, led::Effect::kNormBlink, true};

        // Watching but not answering — the same cyan family, quieter.
        case State::kWatching:
            return {led::colour::kCyan, led::Effect::kBeacon, true};

        // **Green, breathing, at the idle ceiling** — asked for by name, and the
        // state this device is in for almost all of its life. Chosen for how it
        // feels to sit next to rather than for how fast it is read: a nine-second
        // Weber-Fechner breath is what a thing that is *fine* looks like, and
        // anything faster would be a device asking for attention it does not
        // need.
        case State::kReady:
            return {led::colour::kGreen, led::Effect::kBreathe, true};
    }
    return {led::colour::kOff, led::Effect::kSolid, true};
}

}  // namespace indicator
