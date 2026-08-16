#include "wifi_policy.h"

namespace wifimgr {

void Policy::Configure(const Settings &settings, uint32_t now_ms) {
    settings_ = settings;
    if (settings_.network_count > kMaxNetworks) {
        settings_.network_count = kMaxNetworks;
    }

    // The list has been edited. Two things follow, and both are about not
    // acting on stale information: the sticky auth failures go (the likeliest
    // reason anybody edited this is the password that was refused), and so
    // does the memory of which index worked, because index 2 may now be a
    // different network entirely.
    ClearAuthFailures();
    last_good_ = kNoNetwork;
    round_ = 0;

    if (desired_ == Desired::kClient) {
        RestartClient(now_ms);
    }
}

void Policy::SetDesired(Desired desired, uint32_t now_ms) {
    // Idempotent on purpose: the manager reads this out of `config.json` on
    // every pass of its loop, and re-asking for the mode that is already
    // running must not drop a working connection to start it again.
    if (desired == desired_) {
        return;
    }
    desired_ = desired;

    switch (desired) {
        case Desired::kOff:
            state_ = State::kOff;
            network_ = kNoNetwork;
            round_ = 0;
            pending_ = Action::kStop;
            break;
        case Desired::kAp:
            state_ = State::kAp;
            ap_clients_ = 0;
            pending_ = Action::kStartAp;
            break;
        case Desired::kClient:
            RestartClient(now_ms);
            break;
    }
}

void Policy::OnConnected(uint32_t now_ms) {
    (void)now_ms;
    // Only an attempt can succeed. An answer arriving in any other state is
    // about a question nobody is asking any more — the operator switched the
    // radio to AP mode while the association was in flight, most likely.
    if (state_ != State::kConnecting) {
        return;
    }
    state_ = State::kOnline;
    failure_ = Failure::kNone;
    last_good_ = network_;
    round_ = 0;
    // It answered, so whatever it did last time is not what it is doing now.
    if (network_ < kMaxNetworks) {
        auth_failed_ = static_cast<uint8_t>(auth_failed_ & ~(1u << network_));
    }
}

void Policy::OnFailed(Failure why, uint32_t now_ms) {
    failure_ = why;

    if (state_ == State::kOnline) {
        // **A drop is not a refusal.** This link worked a moment ago, so the
        // reason code — which on a marginal link is often a handshake timeout,
        // indistinguishable from a wrong password — must not strike a working
        // network off the list. Go back to it first (§10.9: last-successful
        // first) and start the cycle over.
        round_ = 0;
        network_ = last_good_;
        EnterWaiting(settings_.attempt_gap_ms, now_ms);
        return;
    }

    if (state_ != State::kConnecting) {
        return;
    }

    if (why == Failure::kAuth && network_ < kMaxNetworks) {
        // §10.9: sticky, and reported. Cleared when the fallback AP window
        // ends, because that window is the chance somebody had to fix it.
        auth_failed_ = static_cast<uint8_t>(auth_failed_ | (1u << network_));
    }
    AdvanceAfterFailure(now_ms);
}

void Policy::OnApClients(uint8_t count, uint32_t now_ms) {
    const uint8_t before = ap_clients_;
    ap_clients_ = count;

    // The falling edge, and only it: the manager reports the count on every
    // pass, so restarting the window on "count is zero" would restart it
    // forever and the fallback AP would never expire.
    if (state_ == State::kApWindow && before > 0 && count == 0) {
        window_started_ms_ = now_ms;
    }
}

Action Policy::Tick(uint32_t now_ms) {
    switch (state_) {
        case State::kConnecting:
            if (now_ms - attempt_started_ms_ >= settings_.connect_timeout_ms) {
                // The driver has said nothing at all. On a board that is an AP
                // that associates and never finishes; without this the device
                // never reaches its second network.
                failure_ = Failure::kOther;
                AdvanceAfterFailure(now_ms);
            }
            break;

        case State::kWaiting:
            if (now_ms - wait_started_ms_ >= wait_ms_) {
                BeginAttempt(now_ms);
            }
            break;

        case State::kApWindow:
            if (ApWindowRemainingMs(now_ms) == 0) {
                // Nobody came. Clear the refusals — the window was the chance
                // to fix them — and start the cycle again from the top.
                ClearAuthFailures();
                round_ = 0;
                network_ = kNoNetwork;
                BeginAttempt(now_ms);
            }
            break;

        case State::kOff:
        case State::kOnline:
        case State::kAp:
            // Nothing with a clock in it. `kOnline` in particular: the connect
            // timeout that was running a moment ago belongs to `kConnecting`.
            break;
    }

    const Action action = pending_;
    pending_ = Action::kNone;
    return action;
}

bool Policy::AuthFailed(uint8_t index) const {
    if (index >= kMaxNetworks) {
        return false;
    }
    return (auth_failed_ & (1u << index)) != 0;
}

bool Policy::NoCandidates() const { return NextCandidateFrom(0) == kNoNetwork; }

uint32_t Policy::ApWindowRemainingMs(uint32_t now_ms) const {
    // Somebody is on it: they are the entire reason the window exists, so the
    // clock stops. And with no networks configured at all there is nothing to
    // expire *into* — §10.9's NO_CREDENTIALS, where being findable is not a
    // fallback but the state itself.
    if (ap_clients_ > 0 || settings_.network_count == 0) {
        return kHeldOpen;
    }
    const uint32_t elapsed = now_ms - window_started_ms_;
    if (elapsed >= settings_.ap_window_ms) {
        return 0;
    }
    return settings_.ap_window_ms - elapsed;
}

uint32_t Policy::WaitRemainingMs(uint32_t now_ms) const {
    const uint32_t elapsed = now_ms - wait_started_ms_;
    return elapsed >= wait_ms_ ? 0 : wait_ms_ - elapsed;
}

// --- the transitions --------------------------------------------------------

void Policy::EnterWaiting(uint32_t delay_ms, uint32_t now_ms) {
    state_ = State::kWaiting;
    wait_started_ms_ = now_ms;
    wait_ms_ = delay_ms;
}

void Policy::EnterApWindow(uint32_t now_ms) {
    state_ = State::kApWindow;
    window_started_ms_ = now_ms;
    ap_clients_ = 0;
    pending_ = Action::kStartAp;
}

void Policy::BeginAttempt(uint32_t now_ms) {
    // The index may have gone stale between being chosen and being used — the
    // list can shrink, and a network can be struck off by a refusal in the
    // meantime.
    if (network_ >= settings_.network_count || AuthFailed(network_)) {
        network_ = NextCandidateFrom(0);
    }
    if (network_ == kNoNetwork) {
        EnterApWindow(now_ms);
        return;
    }
    state_ = State::kConnecting;
    attempt_started_ms_ = now_ms;
    pending_ = Action::kStartClient;
}

void Policy::RestartClient(uint32_t now_ms) {
    round_ = 0;
    // §10.9's "try last-successful first": a desk device that moves between a
    // home and an office spends most of its life arriving somewhere it has
    // been before.
    network_ = last_good_;
    BeginAttempt(now_ms);
}

void Policy::AdvanceAfterFailure(uint32_t now_ms) {
    const uint8_t next =
        network_ == kNoNetwork ? kNoNetwork : NextCandidateFrom(static_cast<uint8_t>(network_ + 1));
    if (next != kNoNetwork) {
        network_ = next;
        // Through a wait, never straight into the next attempt: §10.9's
        // "never becomes a tight loop" is enforced by there being no edge out
        // of a failure that does not pass through a clock.
        EnterWaiting(settings_.attempt_gap_ms, now_ms);
        return;
    }

    // The list is exhausted, which is what a round is.
    //
    // **The increment comes first, and that is what makes `rounds: 0`
    // harmless** — the comparison is never made with `round_` at zero, so the
    // networks always get one full pass before the access point goes up. This
    // used to be a `max(1, …)` helper; a mutation that removed it changed no
    // test and no behaviour, which is the honest way to find out that a guard
    // was guarding nothing.
    ++round_;
    if (round_ >= settings_.rounds_before_ap || NoCandidates()) {
        // Either we have tried enough, or there is nothing left to try —
        // and the second case does not wait out the remaining rounds. A
        // device sulking in silence for a minute when it could be findable
        // is a device nobody can fix.
        EnterApWindow(now_ms);
        return;
    }

    network_ = NextCandidateFrom(0);
    EnterWaiting(BackoffForRound(round_), now_ms);
}

uint8_t Policy::NextCandidateFrom(uint8_t start) const {
    for (uint8_t i = start; i < settings_.network_count; ++i) {
        if (!AuthFailed(i)) {
            return i;
        }
    }
    return kNoNetwork;
}

uint32_t Policy::BackoffForRound(uint8_t round) const {
    // Grows with the round and is capped. Both halves matter: without the
    // growth a dead AP is hammered, and without the cap a device that was away
    // for an hour takes an hour to notice it is back.
    const uint32_t scaled = settings_.round_backoff_ms * round;
    if (round != 0 && scaled / round != settings_.round_backoff_ms) {
        return settings_.max_backoff_ms;  // the multiply wrapped
    }
    return scaled > settings_.max_backoff_ms ? settings_.max_backoff_ms : scaled;
}

void Policy::ClearAuthFailures() { auth_failed_ = 0; }

// --- names ------------------------------------------------------------------

const char *Name(Desired desired) {
    switch (desired) {
        case Desired::kOff:
            return "off";
        case Desired::kClient:
            return "client";
        case Desired::kAp:
            return "ap";
    }
    return "?";
}

const char *Name(State state) {
    switch (state) {
        case State::kOff:
            return "off";
        case State::kConnecting:
            return "connecting";
        case State::kWaiting:
            return "waiting";
        case State::kOnline:
            return "connected";
        case State::kApWindow:
            return "temporary ap";
        case State::kAp:
            return "ap";
    }
    return "?";
}

const char *Name(Failure failure) {
    switch (failure) {
        case Failure::kNone:
            return "none";
        case Failure::kAuth:
            return "wrong password";
        case Failure::kNotFound:
            return "no such network";
        case Failure::kOther:
            return "disconnected";
    }
    return "?";
}

}  // namespace wifimgr
