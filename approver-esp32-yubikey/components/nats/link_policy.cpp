#include "link_policy.h"

namespace nats {

const char *Name(State state) {
    switch (state) {
        case State::kOff:
            return "off";
        case State::kNoNetwork:
            return "no network";
        case State::kConnecting:
            return "connecting";
        case State::kWaiting:
            return "waiting";
        case State::kConnected:
            return "connected";
    }
    return "?";
}

void LinkPolicy::Configure(const LinkSettings &settings, uint32_t now_ms) {
    settings_ = settings;

    // Floors rather than trust, the way `SyncPolicy::Configure` states it:
    // these do not come out of `config.json` today, and the day one of them
    // does, a zero here would be a socket opened as fast as the core can run.
    if (settings_.retry_ms < 100u) {
        settings_.retry_ms = 100u;
    }
    if (settings_.retry_max_ms < settings_.retry_ms) {
        settings_.retry_max_ms = settings_.retry_ms;
    }

    if (!connected_ && !awaiting_) {
        // Nothing is up, so nothing is invalidated: start clean and take the
        // first opportunity. A connection that *is* up is dealt with by `Tick`,
        // which is where "switched off" turns into a disconnect.
        backoff_ms_ = settings_.retry_ms;
        next_at_ms_ = now_ms;
    }
}

void LinkPolicy::OnNetwork(bool up, uint32_t now_ms) {
    if (up == network_) {
        return;
    }
    network_ = up;
    if (!up) {
        // Nothing to cancel here: an attempt that is outstanding will answer
        // or time out on its own, and `Tick` is what tears down a connection
        // that is up. What must not happen is a reconnect into a dead route.
        return;
    }
    // A network appearing is not a moment to serve out a wait that was earned
    // against a server nothing could reach.
    backoff_ms_ = settings_.retry_ms;
    next_at_ms_ = now_ms;
}

void LinkPolicy::ConnectNow(uint32_t now_ms) {
    backoff_ms_ = settings_.retry_ms;
    next_at_ms_ = now_ms;
}

void LinkPolicy::Restart(uint32_t now_ms) {
    if (connected_ || awaiting_) {
        // Handled in `Tick`, so that the disconnect and the attempt that
        // follows it happen in the caller's order rather than this one's.
        restart_ = true;
        return;
    }
    ConnectNow(now_ms);
}

Action LinkPolicy::Tick(uint32_t now_ms) {
    if (connected_ || awaiting_) {
        const bool must_let_go = !settings_.enabled || !network_ || restart_;
        if (!must_let_go) {
            return Action::kNone;
        }
        connected_ = false;
        awaiting_ = false;
        restart_ = false;
        // Whatever the last attempt earned belonged to the old circumstances.
        // Due immediately, so that a changed address reconnects on the next
        // pass and a network that comes back is not made to wait.
        backoff_ms_ = settings_.retry_ms;
        next_at_ms_ = now_ms;
        return Action::kDisconnect;
    }

    // **A restart that outlived what it was asked about**, which from here is
    // simply "try now" — the same thing `Restart` does when it is called with
    // nothing up. Consuming it here rather than only in the branch above is
    // what the board caught: an address changed mid-attempt left the flag set,
    // and the next connection that *worked* was torn down on its first tick.
    if (restart_) {
        restart_ = false;
        backoff_ms_ = settings_.retry_ms;
        next_at_ms_ = now_ms;
    }

    if (!settings_.enabled || !network_) {
        return Action::kNone;
    }
    if (!Reached(now_ms, next_at_ms_)) {
        return Action::kNone;
    }
    awaiting_ = true;
    return Action::kConnect;
}

void LinkPolicy::OnResult(bool ok, uint32_t now_ms) {
    if (!awaiting_) {
        return;
    }
    awaiting_ = false;

    if (ok) {
        connected_ = true;
        connected_at_ms_ = now_ms;
        if (connects_ < 0xFFFFu) {
            ++connects_;
        }
        failures_ = 0;
        // What was learned about a server that has just answered is stale.
        backoff_ms_ = settings_.retry_ms;
        return;
    }

    if (failures_ < 0xFFFFu) {
        ++failures_;
    }
    next_at_ms_ = now_ms + backoff_ms_;
    // Grown after it is used, and capped: a device pointed at a bus that is
    // switched off settles into asking once a minute rather than continuously.
    backoff_ms_ =
        backoff_ms_ > settings_.retry_max_ms / 2u ? settings_.retry_max_ms : backoff_ms_ * 2u;
}

void LinkPolicy::OnDropped(uint32_t now_ms) {
    if (!connected_) {
        return;
    }
    connected_ = false;
    if (drops_ < 0xFFFFu) {
        ++drops_;
    }
    // **Not a refusal**, so the backoff starts from the bottom — a server that
    // accepted us and then restarted is not an address that is wrong. And not
    // instant either: reconnecting the moment the socket closed turns a server
    // that is kicking us into a loop running as fast as lwIP can open sockets.
    backoff_ms_ = settings_.retry_ms;
    next_at_ms_ = now_ms + settings_.retry_ms;
}

State LinkPolicy::CurrentState() const {
    if (connected_) {
        return State::kConnected;
    }
    if (awaiting_) {
        return State::kConnecting;
    }
    if (!settings_.enabled) {
        return State::kOff;
    }
    if (!network_) {
        return State::kNoNetwork;
    }
    return State::kWaiting;
}

uint32_t LinkPolicy::NextAttemptInMs(uint32_t now_ms) const {
    if (connected_ || !settings_.enabled || !network_) {
        // Nothing is scheduled, rather than scheduled for never — the
        // distinction the console prints as "connected", "off" and "no
        // network".
        return kNever;
    }
    if (awaiting_ || Reached(now_ms, next_at_ms_)) {
        return 0;
    }
    return next_at_ms_ - now_ms;
}

uint32_t LinkPolicy::ConnectedForMs(uint32_t now_ms) const {
    return connected_ ? now_ms - connected_at_ms_ : 0;
}

}  // namespace nats
