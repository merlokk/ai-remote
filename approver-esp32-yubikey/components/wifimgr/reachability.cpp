#include "reachability.h"

namespace wifimgr {

void Reachability::Configure(const ProbeSettings &settings, uint32_t now_ms) {
    settings_ = settings;
    if (settings_.target_count > kMaxProbeTargets) {
        settings_.target_count = kMaxProbeTargets;
    }

    // The list may be a different list. What was learned about the old one —
    // which address answers, how many rounds have failed — is about addresses
    // that may not be in it any more.
    state_ = Internet::kUnknown;
    preferred_ = 0;
    failed_rounds_ = 0;
    awaiting_result_ = false;
    target_ = kNoTarget;
    attempts_ = 0;

    if (link_up_) {
        ProbeNow(now_ms);
    } else {
        round_due_ = false;
    }
}

void Reachability::LinkUp(uint32_t now_ms) {
    if (link_up_) {
        return;
    }
    link_up_ = true;
    // At once. A device that has just joined a network and cannot say whether
    // it is any good is a device somebody is about to ask about.
    ProbeNow(now_ms);
}

void Reachability::LinkDown(uint32_t now_ms) {
    (void)now_ms;
    link_up_ = false;
    round_due_ = false;
    attempts_ = 0;
    target_ = kNoTarget;
    // **Unknown, not offline.** The absence of a link is already on the screen
    // as the Wi-Fi state; repeating it as "no internet" would put two marks
    // there for one problem and would claim something never established.
    state_ = Internet::kUnknown;
    failed_rounds_ = 0;
    // Anything outstanding is now about a link that is gone.
    awaiting_result_ = false;
}

void Reachability::ProbeNow(uint32_t now_ms) {
    if (!link_up_) {
        return;
    }
    attempts_ = 0;
    round_due_ = true;
    wait_started_ms_ = now_ms;
}

Probe Reachability::Tick(uint32_t now_ms) {
    if (!settings_.enabled || !link_up_ || settings_.target_count == 0) {
        return Probe::kNone;
    }
    if (awaiting_result_) {
        // The manager is still waiting on `esp_ping`. A second request would
        // be a second session against the one handle it keeps.
        return Probe::kNone;
    }
    if (!round_due_ && now_ms - wait_started_ms_ >= settings_.interval_ms) {
        StartRound(now_ms);
    }
    if (!round_due_) {
        return Probe::kNone;
    }

    target_ = NthTarget(attempts_);
    if (target_ == kNoTarget) {
        round_due_ = false;
        return Probe::kNone;
    }
    awaiting_result_ = true;
    return Probe::kSend;
}

void Reachability::OnResult(bool reachable, uint32_t now_ms) {
    if (!awaiting_result_) {
        // A late answer to a question nobody is asking any more — the link
        // dropped, or the settings changed, while `esp_ping` was still out.
        // Letting it through would put a dead link back online.
        return;
    }
    awaiting_result_ = false;

    if (reachable) {
        // One reply is proof. No hysteresis on the way back, unlike the way
        // out — an outage that has ended is over.
        preferred_ = target_;
        state_ = Internet::kOnline;
        failed_rounds_ = 0;
        have_success_ = true;
        last_success_ms_ = now_ms;
        ScheduleNextRound(now_ms);
        return;
    }

    ++attempts_;
    if (attempts_ < settings_.target_count) {
        // Straight on to the next address, with no wait: one host being
        // blocked is not the internet being down, and spending a whole
        // interval per target would make a three-address list take three
        // minutes to conclude anything.
        return;
    }

    // Every address in the list ignored us. That is one failed round.
    if (failed_rounds_ < 0xFF) {
        ++failed_rounds_;
    }
    if (failed_rounds_ >= settings_.failures_before_offline) {
        state_ = Internet::kOffline;
    }
    ScheduleNextRound(now_ms);
}

uint32_t Reachability::SinceLastSuccessMs(uint32_t now_ms) const {
    // Not zero, which would read as "a moment ago" — the most wrong of the
    // available answers.
    return have_success_ ? now_ms - last_success_ms_ : kNeverSucceeded;
}

uint32_t Reachability::NextProbeInMs(uint32_t now_ms) const {
    if (!settings_.enabled || !link_up_ || settings_.target_count == 0) {
        return kNeverSucceeded;
    }
    if (round_due_ || awaiting_result_) {
        return 0;
    }
    const uint32_t elapsed = now_ms - wait_started_ms_;
    return elapsed >= settings_.interval_ms ? 0 : settings_.interval_ms - elapsed;
}

void Reachability::StartRound(uint32_t now_ms) {
    attempts_ = 0;
    round_due_ = true;
    wait_started_ms_ = now_ms;
}

void Reachability::ScheduleNextRound(uint32_t now_ms) {
    round_due_ = false;
    attempts_ = 0;
    wait_started_ms_ = now_ms;
}

uint8_t Reachability::NthTarget(uint8_t attempt) const {
    if (attempt >= settings_.target_count) {
        return kNoTarget;
    }
    // The one that answered last goes first; the rest follow in order,
    // wrapping. Same idea as the network policy's last-successful-first, and
    // it stops every round from opening with a host this network drops.
    return static_cast<uint8_t>((preferred_ + attempt) % settings_.target_count);
}

const char *Name(Internet state) {
    switch (state) {
        case Internet::kUnknown:
            return "unknown";
        case Internet::kOnline:
            return "online";
        case Internet::kOffline:
            return "offline";
    }
    return "?";
}

}  // namespace wifimgr
