#pragma once

// Is there an internet on the other side of this link (CLAUDE.md §10.9)?
//
// **A different question from "am I connected", and the device needs both.**
// Associating with an access point and holding an address says the radio is
// working; it says nothing about whether anything is reachable through it. A
// router with no uplink, a captive portal, a guest network that only allows
// port 80 — all of them look exactly like a healthy connection from the
// station's side. So: once a minute, while there is a link, ping one of a few
// addresses from `config.json` and see.
//
// **This file includes `<cstdint>` and nothing else**, the third one in this
// firmware to do so (`ui/navigator.h`, `wifi_policy.h`). The scheduling, the
// rotation over targets and the hysteresis are all here and all run under
// Unity with no board; sending an actual ICMP echo is four lines of
// `esp_ping` in the manager. The rule §10.14.2 states, applied again: the
// decision is testable, the syscall is not, so they live apart.
//
// What it deliberately does **not** do is feed back into `wifi_policy.h`. A
// network that associates and carries no traffic is a fact to *report*, not a
// reason to drop the link and try the next one — that would turn one dead
// uplink into a device that cycles through its networks forever, and the
// operator would see a device that cannot connect rather than a router that
// needs rebooting. §10.9's screen shows both; only one of them chooses.

#include <cstdint>

namespace wifimgr {

// Enough to spread the question across a few operators. Four is the same
// number `config::kMaxNetworks` uses, and for the same reason: past that it is
// somebody else's problem.
inline constexpr uint8_t kMaxProbeTargets = 4;

inline constexpr uint8_t kNoTarget = 0xFF;

// What `SinceLastSuccessMs` answers when nothing has ever answered.
inline constexpr uint32_t kNeverSucceeded = 0xFFFFFFFFu;

// **Three states, not two, and the third one is the honest one.** "I have not
// looked" is not "there is no internet" — a device that reported offline
// because it had just connected and had not asked yet would be lying with a
// straight face.
enum class Internet : uint8_t {
    kUnknown,  // no link, checking switched off, or the first round is still out
    kOnline,
    kOffline,
};

enum class Probe : uint8_t {
    kNone,
    kSend,  // ping `Target()`, then call `OnResult`
};

struct ProbeSettings {
    bool enabled = true;
    uint8_t target_count = 0;

    // Between rounds. §10.9's minute — long enough to be free, short enough
    // that an uplink coming back is noticed before anybody reaches for the
    // router.
    uint32_t interval_ms = 60000;

    // **Consecutive failed rounds before saying offline.** One is too eager: a
    // single lost round is a lost packet, a roaming beacon, a router that was
    // busy. Going *online* needs no hysteresis at all — one reply is proof,
    // and the asymmetry is deliberate.
    uint8_t failures_before_offline = 2;
};

class Reachability {
   public:
    Reachability() = default;
    Reachability(const Reachability &) = delete;
    Reachability &operator=(const Reachability &) = delete;

    void Configure(const ProbeSettings &settings, uint32_t now_ms);

    // The link came up as a client, or went away. `LinkDown` puts the state
    // back to `kUnknown` rather than to `kOffline`, because those are
    // different facts and the screen must not spell them the same way.
    void LinkUp(uint32_t now_ms);
    void LinkDown(uint32_t now_ms);
    bool LinkIsUp() const { return link_up_; }

    // Ask now rather than at the next interval — the console's "check", and
    // what a freshly established link gets for free.
    void ProbeNow(uint32_t now_ms);

    // The pump. `kSend` means: ping `Target()` and hand the answer back.
    // Nothing else is returned while a probe is outstanding.
    Probe Tick(uint32_t now_ms);

    // The result of the probe `Tick` last asked for. A late answer that
    // arrives after the link dropped is ignored — the state machine moved on,
    // and a reply to a question nobody is asking any more must not put a dead
    // link back online.
    void OnResult(bool reachable, uint32_t now_ms);

    Internet State() const { return state_; }
    uint8_t Target() const { return target_; }

    // Consecutive failed rounds. The screen wants this as much as the state:
    // "offline" and "one round short of saying offline" look the same
    // otherwise.
    uint8_t FailedRounds() const { return failed_rounds_; }

    uint32_t SinceLastSuccessMs(uint32_t now_ms) const;
    uint32_t NextProbeInMs(uint32_t now_ms) const;
    bool Probing() const { return awaiting_result_; }

   private:
    void StartRound(uint32_t now_ms);
    void ScheduleNextRound(uint32_t now_ms);
    uint8_t NthTarget(uint8_t attempt) const;

    ProbeSettings settings_ = {};
    Internet state_ = Internet::kUnknown;
    bool link_up_ = false;

    uint8_t target_ = kNoTarget;
    // The one that answered last, tried first next time — the same idea
    // `wifi_policy.h` applies to networks, and here it keeps a round from
    // spending its first probe on a host that is blocked every single time.
    uint8_t preferred_ = 0;
    uint8_t attempts_ = 0;  // targets tried in this round
    uint8_t failed_rounds_ = 0;

    bool awaiting_result_ = false;
    bool round_due_ = false;
    bool have_success_ = false;

    uint32_t wait_started_ms_ = 0;
    uint32_t last_success_ms_ = 0;
};

const char *Name(Internet state);

}  // namespace wifimgr
