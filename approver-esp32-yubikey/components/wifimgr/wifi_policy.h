#pragma once

// Which network the radio should be on, and why (CLAUDE.md §10.9).
//
// **This file includes nothing but `<cstdint>`.** That is the same trick
// `ui/navigator.h` plays and it is here for the same reason: §10.11 makes the
// host tier the comprehensive one, and a Wi-Fi state machine that reaches for
// `esp_wifi.h` is a machine that can only be tested on a board. So the
// *decision* — connect to network 2, raise the access point, wait 5 seconds —
// lives here, and carrying it out is `wifi::Radio`'s job (`components/wifi`).
//
// The split is the one the whole component pair is built on:
//
//   * the **driver** knows how to join one network, how to be an access point,
//     what the link is doing, and what is on the air. It has no opinion about
//     which of those should be happening;
//   * the **policy** has nothing but that opinion. It never touches a radio.
//
// The shape §10.9 asks for, with the round-robin the driver's single-network
// view cannot express:
//
//     NO_CREDENTIALS ──join──► CONNECTING ──got IP──► ONLINE ──drop──► RETRYING
//
// and, on top of it, what a device with no keyboard actually needs: try each
// remembered network in turn; after a configured number of fruitless rounds
// stop trying and **become** an access point for a couple of minutes, so an
// operator can reach the thing and tell it about a network that exists; if
// nobody turns up, go back to trying. Repeat forever, never in a tight loop.
//
// Two states, therefore, and the distinction is the point:
//
//   * **desired** — AP or client, what the operator asked for (`config.json`,
//     the console, later §10.8.6);
//   * **current** — `client "point3" connected`, or `temporary AP` — what is
//     actually happening on the way there.
//
// Timing is milliseconds in `uint32_t` and is only ever *subtracted*, so the
// wrap at ~49 days is arithmetic rather than a case to handle. `buttons.h`
// makes the same choice and the same note.

#include <cstdint>

namespace wifimgr {

// Must match `config::kMaxNetworks`. It is not included from there on purpose —
// see the top of this file — so the two are tied by a `static_assert` in
// `wifi_manager.cpp`, which is the one place allowed to know about both.
inline constexpr uint8_t kMaxNetworks = 4;

// What `NextNetwork()` answers when there is nothing worth trying: no
// remembered networks at all, or every one of them refused our password.
inline constexpr uint8_t kNoNetwork = 0xFF;

// What `ApWindowRemainingMs()` answers when the window has no end — a station
// is attached and being useful, or there is nothing to go back to.
inline constexpr uint32_t kHeldOpen = 0xFFFFFFFFu;

// What the operator asked for. Not what is happening.
enum class Desired : uint8_t {
    kOff,     // radio down; the device is a clock and nothing else
    kClient,  // join one of the remembered networks
    kAp,      // be an access point, permanently — not the fallback below
};

// What is happening. The two AP states are deliberately different values: one
// is the answer to a request and the other is a symptom, and a screen that
// spelled them the same way would be lying about whether anything is wrong.
enum class State : uint8_t {
    kOff,
    kConnecting,  // an attempt is in flight against `Network()`
    kWaiting,     // between attempts — the backoff §10.9 requires
    kOnline,      // joined, and `Network()` is the one we are on
    kApWindow,    // the temporary AP: nothing would have us, so we are findable
    kAp,          // an access point because that is what was asked for
};

// Why the last attempt ended. §10.9: "wrong password and 'the AP is out of
// range' are different problems and the screen must not spell them the same
// way". Only `kAuth` is sticky.
enum class Failure : uint8_t {
    kNone,
    kAuth,      // the password was refused
    kNotFound,  // no such network on the air
    kOther,     // a drop, a timeout, anything the driver could not classify
};

// What the policy wants done to the radio, right now. One per `Tick`, and
// `kNone` almost always.
enum class Action : uint8_t {
    kNone,
    kStop,         // radio down
    kStartAp,      // become an access point
    kStartClient,  // join `Network()`
};

// Everything tunable, in one struct so a test can shorten a two-minute window
// to twenty milliseconds and `config.json` can say `rounds: 3`.
struct Settings {
    // How many networks `Network()` may name. Not the networks themselves: the
    // policy never sees an SSID, which is what keeps a password out of a state
    // machine that gets printed in logs.
    uint8_t network_count = 0;

    // Full passes over the list before the fallback AP goes up. §10.9's "2-3".
    // 0 behaves as 1 rather than as "never try": the round is counted before
    // it is compared, so every network gets one attempt whatever this says.
    uint8_t rounds_before_ap = 2;

    // How long an attempt may stay in flight with the driver saying nothing.
    // Without this a driver that never answers is a device that never gets to
    // its second network.
    uint32_t connect_timeout_ms = 15000;

    // Between two attempts inside one round. Short: these are different
    // networks, not a retry of the same one.
    uint32_t attempt_gap_ms = 1000;

    // Between rounds, multiplied by the round number and capped — §10.9's
    // "a few seconds, then tens, capped at a minute or so", and its "never
    // becomes a tight loop".
    uint32_t round_backoff_ms = 5000;
    uint32_t max_backoff_ms = 60000;

    // How long the fallback AP stays up with nobody on it.
    uint32_t ap_window_ms = 120000;
};

class Policy {
   public:
    Policy() = default;
    Policy(const Policy &) = delete;
    Policy &operator=(const Policy &) = delete;

    // The settings, and a restart of whatever is in progress: this is called
    // when the network list has been edited, and an attempt against the old
    // list number 2 is an attempt against a network that may no longer be
    // there. It also clears the sticky auth failures — the most likely reason
    // somebody edited the file is that one of them was the point.
    void Configure(const Settings &settings, uint32_t now_ms);

    // Idempotent: called with the value it already has, nothing restarts.
    // That matters because the manager reads this out of `config.json` on
    // every pass of its loop.
    void SetDesired(Desired desired, uint32_t now_ms);
    Desired GetDesired() const { return desired_; }

    // --- what the radio reports ------------------------------------------
    void OnConnected(uint32_t now_ms);
    void OnFailed(Failure why, uint32_t now_ms);

    // How many stations are attached to our AP. **This is what decides whether
    // the fallback window ends**: somebody is here, so the two minutes stop
    // running (§10.9's screen is what they came for). When the last one leaves,
    // the window starts again from the beginning rather than resuming — a
    // station that joined at 1:59 should not leave the operator four seconds.
    void OnApClients(uint8_t count, uint32_t now_ms);

    // The pump. Runs the timers and returns the one thing to do, if any.
    Action Tick(uint32_t now_ms);

    // --- what is happening -------------------------------------------------
    State GetState() const { return state_; }

    // The index the last `kStartClient` named, and the one `kOnline` is on.
    // `kNoNetwork` when there is nothing to name.
    uint8_t Network() const { return network_; }

    // Which pass over the list we are on, 0-based. Reset by a success, by the
    // AP window, and by `Configure`.
    uint8_t Round() const { return round_; }

    // Why the last attempt ended, kept until the next one starts.
    Failure LastFailure() const { return failure_; }

    // §10.9: an auth failure is **sticky and is reported, not retried
    // forever**. A network that refused our password is skipped for the rest
    // of the cycle and cleared when the AP window ends — because that window
    // is exactly the opportunity somebody had to fix the password.
    bool AuthFailed(uint8_t index) const;

    // Milliseconds until the fallback AP gives up and goes back to trying, or
    // `kHeldOpen`. Meaningless outside `kApWindow`.
    uint32_t ApWindowRemainingMs(uint32_t now_ms) const;

    // Milliseconds until the next attempt. Meaningless outside `kWaiting`.
    uint32_t WaitRemainingMs(uint32_t now_ms) const;

    // True when the client path has nowhere to go **right now** — no networks
    // at all, or every one of them refused the password. It is what sends the
    // fallback AP up without spending the configured rounds on a list that has
    // nothing left in it.
    //
    // The two halves of it end differently, which is why the window's own rule
    // is separate: a list that is all sticky failures gets another chance when
    // the window expires and clears them, and a list that is **empty** does
    // not — §10.9's NO_CREDENTIALS is a first-class state, and there the AP
    // stays up rather than expiring back into having nothing to try.
    bool NoCandidates() const;

   private:
    void Enter(State state, uint32_t now_ms);
    void EnterWaiting(uint32_t delay_ms, uint32_t now_ms);
    void EnterApWindow(uint32_t now_ms);
    void BeginAttempt(uint32_t now_ms);
    void RestartClient(uint32_t now_ms);
    void AdvanceAfterFailure(uint32_t now_ms);
    uint8_t NextCandidateFrom(uint8_t start) const;
    uint32_t BackoffForRound(uint8_t round) const;
    void ClearAuthFailures();

    Settings settings_ = {};
    Desired desired_ = Desired::kOff;
    State state_ = State::kOff;
    Action pending_ = Action::kNone;

    uint8_t network_ = kNoNetwork;
    // The one that worked last. §10.9: "try last-successful first" — a desk
    // device that moves between a home and an office spends most of its life
    // arriving somewhere it has been before.
    uint8_t last_good_ = kNoNetwork;
    uint8_t round_ = 0;
    uint8_t auth_failed_ = 0;  // one bit per network, hence kMaxNetworks <= 8
    static_assert(kMaxNetworks <= 8, "auth_failed_ is a bitmask in a uint8_t");

    uint8_t ap_clients_ = 0;
    Failure failure_ = Failure::kNone;

    uint32_t attempt_started_ms_ = 0;
    uint32_t wait_started_ms_ = 0;
    uint32_t wait_ms_ = 0;
    uint32_t window_started_ms_ = 0;
};

// For logs and the console. Free functions rather than methods: they are about
// the enums, not about an instance.
const char *Name(Desired desired);
const char *Name(State state);
const char *Name(Failure failure);

}  // namespace wifimgr
