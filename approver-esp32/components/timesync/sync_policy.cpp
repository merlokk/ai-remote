#include "sync_policy.h"

namespace timesync {

void SyncPolicy::Configure(const SyncSettings &settings, uint32_t now_ms) {
    settings_ = settings;

    // Floors rather than trust: this struct is filled from `config.json`, and
    // an interval of zero would be a poll loop against somebody else's NTP
    // server with a friendly name on it. The file's own parser clamps too —
    // this is the half that cannot be edited out.
    if (settings_.interval_ms < 1000u) {
        settings_.interval_ms = 1000u;
    }
    if (settings_.retry_ms < 1000u) {
        settings_.retry_ms = 1000u;
    }
    if (settings_.retry_max_ms < settings_.retry_ms) {
        settings_.retry_max_ms = settings_.retry_ms;
    }
    backoff_ms_ = settings_.retry_ms;

    // **What has already happened is kept** — see the header. A device that
    // synced twenty minutes ago has a good clock whatever the interval is now,
    // so this reschedules and nothing more.
    next_at_ms_ = have_sync_ ? last_sync_ms_ + settings_.interval_ms : now_ms;
}

void SyncPolicy::OnInternet(bool usable, uint32_t now_ms) {
    if (usable == usable_) {
        return;
    }
    usable_ = usable;
    if (!usable_) {
        // Nothing to cancel: an exchange still outstanding will answer or time
        // out on its own, and the schedule is about when to ask next rather
        // than about the link.
        return;
    }

    if (!have_sync_) {
        // Never synced, so it is due — the boot case, and the reason there is
        // no separate one.
        next_at_ms_ = now_ms;
        return;
    }
    if (Reached(now_ms, last_sync_ms_ + settings_.min_gap_ms)) {
        // Long enough off the air that the clock may have moved. `Earlier`
        // rather than an assignment: a sync already overdue must not be pushed
        // out by the event that was supposed to trigger one.
        next_at_ms_ = Earlier(next_at_ms_, now_ms);
    }
    // Inside the guard, this is a link that bounced and not news about the
    // time. The schedule is left exactly as it was.
}

void SyncPolicy::SyncNow(uint32_t now_ms) { next_at_ms_ = now_ms; }

Action SyncPolicy::Tick(uint32_t now_ms) {
    if (!settings_.enabled || !usable_ || awaiting_) {
        return Action::kNone;
    }
    if (!Reached(now_ms, next_at_ms_)) {
        return Action::kNone;
    }
    awaiting_ = true;
    return Action::kSync;
}

void SyncPolicy::OnResult(bool ok, uint32_t now_ms) {
    if (!awaiting_) {
        return;
    }
    awaiting_ = false;

    if (ok) {
        have_sync_ = true;
        last_sync_ms_ = now_ms;
        if (successes_ < 0xFFFFu) {
            ++successes_;
        }
        failures_ = 0;
        // What was learned about a server that has just answered is stale.
        backoff_ms_ = settings_.retry_ms;
        next_at_ms_ = now_ms + settings_.interval_ms;
        return;
    }

    if (failures_ < 0xFFFFu) {
        ++failures_;
    }
    next_at_ms_ = now_ms + backoff_ms_;
    // Grown after it is used, and capped: a device with no route to any NTP
    // server settles into asking a few times an hour rather than sixty.
    backoff_ms_ =
        backoff_ms_ > settings_.retry_max_ms / 2u ? settings_.retry_max_ms : backoff_ms_ * 2u;
}

uint32_t SyncPolicy::SinceLastSyncMs(uint32_t now_ms) const {
    return have_sync_ ? now_ms - last_sync_ms_ : kNever;
}

uint32_t SyncPolicy::NextSyncInMs(uint32_t now_ms) const {
    if (!settings_.enabled || !usable_) {
        // Nothing is scheduled, rather than "scheduled for never" — the
        // distinction the console prints as "off" and "no internet".
        return kNever;
    }
    if (awaiting_ || Reached(now_ms, next_at_ms_)) {
        return 0;
    }
    return next_at_ms_ - now_ms;
}

}  // namespace timesync
