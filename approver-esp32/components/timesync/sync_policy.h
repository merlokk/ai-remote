#pragma once

// **When** to ask the network what time it is (CLAUDE.md §10.8.2).
//
// The clock this device keeps is the PCF85063's, adopted at boot and correct
// across a power cut — but it is a watch crystal, so it drifts, and nothing on
// the board can tell by how much. SNTP is what corrects it, and this file is
// the decision about when: a fresh device knows nothing and should ask the
// moment it can, a device that has been running for a day should ask again
// before the drift is visible on a clock face, and neither of those is a reason
// to ask a stranger's server every minute.
//
// So: **at boot, when the internet appears, and every `interval` after that.**
// The first two are the same rule from the state machine's side — there is no
// separate "boot" case, because a device that has just started has never
// synced, and a device that has never synced is due.
//
// **This file includes `<cstdint>` and nothing else**, the fourth in this
// firmware to do so (`ui/navigator.h`, `wifi_policy.h`, `reachability.h`). The
// schedule, the flap guard and the backoff all run under Unity with no board
// (§10.11); resolving a hostname and waiting on a UDP packet is the glue's job
// next door, and it is the part with no decisions in it.
//
// What it deliberately does **not** own is whether there is an internet. That
// question is answered once, by `wifimgr` (§10.9), and this reads the answer —
// the seam `wifi_manager.h` said SNTP would hang off, rather than a second
// probe with its own opinion.

#include <cstdint>

namespace timesync {

// What the ages below answer when there is nothing to age: no sync has ever
// happened, or nothing is scheduled. Not zero, which would read as "just now"
// and "immediately" — the two most wrong answers available.
inline constexpr uint32_t kNever = 0xFFFFFFFFu;

struct SyncSettings {
    bool enabled = true;

    // §10.8.2's six hours, and the one number here that is `config.json`'s.
    // The rest below are the shape of this file rather than a preference —
    // the same call §10.9 makes about its connect timeout.
    uint32_t interval_ms = 6u * 60u * 60u * 1000u;

    // **The flap guard.** A link that comes back is a reason to sync, because
    // the device may have been off the air for a week; a link that comes back
    // four times in a minute is not four reasons. Nothing re-syncs within this
    // of a successful sync, however many times the internet appears.
    uint32_t min_gap_ms = 5u * 60u * 1000u;

    // A failed sync is retried well before the next interval — six hours is
    // the gap between *good* answers, not a punishment for a server that was
    // busy. The wait grows and is capped, so a device with no route to any NTP
    // server settles into asking four times an hour rather than sixty.
    uint32_t retry_ms = 60u * 1000u;
    uint32_t retry_max_ms = 15u * 60u * 1000u;
};

enum class Action : uint8_t {
    kNone,
    kSync,  // ask the server now, then hand the answer back through `OnResult`
};

class SyncPolicy {
   public:
    SyncPolicy() = default;
    SyncPolicy(const SyncPolicy &) = delete;
    SyncPolicy &operator=(const SyncPolicy &) = delete;

    // **Keeps what has already happened.** Changing the interval reschedules
    // the next sync; it does not throw away the fact that the clock was set
    // twenty minutes ago, which is still true. `reachability.h` resets on
    // `Configure` because its list of addresses may be a different list — a
    // sync that succeeded is not about a list.
    void Configure(const SyncSettings &settings, uint32_t now_ms);

    // Whether there is something to ask through. **Not the same question as
    // "is the link up"**, and not this file's to answer: the manager decides,
    // and what it means by `true` is stated where it is computed.
    void OnInternet(bool usable, uint32_t now_ms);
    bool InternetIsUsable() const { return usable_; }

    // Ask at the next opportunity, ignoring both the interval and the flap
    // guard — the console's `date sync`, and the one caller allowed to say
    // "now" without a reason. It still needs an internet: forcing the question
    // does not conjure a server to ask.
    void SyncNow(uint32_t now_ms);

    // The pump. `kSync` means: run one SNTP exchange and call `OnResult`.
    // Nothing else comes back while one is outstanding.
    Action Tick(uint32_t now_ms);

    // The answer to the sync `Tick` last asked for. A result nobody asked for
    // is ignored — the same rule `reachability.h` states, and for the same
    // reason: a late answer must not reschedule a machine that has moved on.
    void OnResult(bool ok, uint32_t now_ms);

    // What the machine is running on, which is not the same as what the file
    // says: the file changes on `config set`, this changes on `Apply`.
    bool Enabled() const { return settings_.enabled; }

    bool Syncing() const { return awaiting_; }
    bool EverSynced() const { return have_sync_; }

    // Since the last **successful** sync, which is the only kind that moved
    // the clock. `kNever` before the first one.
    uint32_t SinceLastSyncMs(uint32_t now_ms) const;

    // `kNever` when nothing is scheduled — switched off, or no internet to ask
    // through. Zero when it is due now or already running.
    uint32_t NextSyncInMs(uint32_t now_ms) const;

    uint16_t Successes() const { return successes_; }

    // Consecutive failures, cleared by one success. The screen and the console
    // want this as much as the state: "it has never worked" and "it missed the
    // last one" look the same otherwise.
    uint16_t FailuresInARow() const { return failures_; }

   private:
    // Wrap-safe throughout: `now_ms` is `esp_timer` milliseconds and rolls over
    // every ~49 days, so every deadline is compared as a signed difference and
    // never as `now >= deadline`.
    static bool Reached(uint32_t now_ms, uint32_t at_ms) {
        return static_cast<int32_t>(now_ms - at_ms) >= 0;
    }

    // The earlier of the two, across the wrap.
    static uint32_t Earlier(uint32_t a_ms, uint32_t b_ms) {
        return static_cast<int32_t>(a_ms - b_ms) < 0 ? a_ms : b_ms;
    }

    SyncSettings settings_ = {};
    bool usable_ = false;
    bool awaiting_ = false;
    bool have_sync_ = false;

    uint32_t next_at_ms_ = 0;
    uint32_t last_sync_ms_ = 0;
    uint32_t backoff_ms_ = 60u * 1000u;

    uint16_t successes_ = 0;
    uint16_t failures_ = 0;
};

}  // namespace timesync
