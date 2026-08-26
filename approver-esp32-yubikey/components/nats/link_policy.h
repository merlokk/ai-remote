#pragma once

// **When** to have a connection to the bus, and when to let one go
// (CLAUDE.md §10.5, §10.9).
//
// The socket itself is next door in `nats_bus.h` and has no decisions in it;
// this is the half that does, and it is one of the files in this firmware whose
// subject includes `<cstdint>` and nothing else (`wifi_policy.h`,
// `reachability.h`, `request_card.h`). That is what puts every
// rule below under Unity with no board and no fake (§10.11) — and the reason
// to draw the line here rather than anywhere else is that the half which
// cannot be tested that way is the half with nothing to decide.
//
// What it owns, and each of these is a sentence from somewhere else in the
// design made executable:
//
//   * **only a network releases it** (§10.9: "only `ONLINE` releases the bus
//     task, and on the way down it tears the socket rather than letting it
//     hang on a dead route"). What counts as a network is the manager's
//     answer, not this file's;
//   * **a refused connection backs off, growing and capped** — the shape
//     `wifi_policy.h` and `sync_policy.h` both have, and for the same reason:
//     a device retrying a server that is not there ten times a second heats
//     up, drains a battery and floods the log;
//   * **a drop is not a refusal.** A server that accepted us and then closed
//     the socket is a server that restarted, not an address that is wrong, so
//     the next attempt starts at the bottom of the backoff — but it is still
//     an attempt later rather than immediately, or a server that is kicking us
//     becomes a loop running as fast as lwIP can open sockets;
//   * **a changed address drops what is up.** `Restart` is that, and it is
//     also what "try now, properly" means.
//
// What it deliberately does **not** own: reconnection inside the client
// library. `debsahu/espidf-nats` has its own backoff, and `nats_bus.cpp`
// switches it off — two things deciding when to reconnect is one thing too
// many, and the one that can be tested is this one.
//
// And nothing here has heard of an approval (§10.14.2): a bus is a bus.

#include <cstdint>

namespace nats {

// What the ages below answer when there is nothing to age. Not zero, which
// reads as "now" — the two most wrong answers available.
inline constexpr uint32_t kNever = 0xFFFFFFFFu;

struct LinkSettings {
    // There is an address to connect to **and** nobody has said "off". One
    // switch rather than two fields that can disagree — the same call
    // `Wifi::active` makes.
    bool enabled = false;

    // The first wait after a refusal, and the ceiling it grows to. Not in
    // `config.json`: the shape of this file rather than a preference, the same
    // call §10.9 makes about its connect timeout. A device whose reconnect
    // interval is operator-settable is a device with one more way to be
    // configured into never working.
    uint32_t retry_ms = 2000;
    uint32_t retry_max_ms = 60000;
};

// What the device is doing about the bus. Five, because "not connected" is
// four different problems and a readout that spelled them the same way would
// be one nobody could act on.
enum class State : uint8_t {
    kOff,         // no address, or switched off at the console
    kNoNetwork,   // nothing to connect through yet
    kConnecting,  // an attempt is outstanding
    kWaiting,     // the last one was refused; the backoff is running
    kConnected,
};

enum class Action : uint8_t {
    kNone,
    kConnect,     // open one, then hand the answer back through `OnResult`
    kDisconnect,  // tear it down now — the network went, or the address did
};

const char *Name(State state);

class LinkPolicy {
   public:
    LinkPolicy() = default;
    LinkPolicy(const LinkPolicy &) = delete;
    LinkPolicy &operator=(const LinkPolicy &) = delete;

    // Switching it off while a connection is up asks for that connection back
    // on the next `Tick`; switching it on takes the first opportunity rather
    // than serving out a backoff nobody is waiting for.
    void Configure(const LinkSettings &settings, uint32_t now_ms);

    // Whether there is something to connect *through* — a client link with an
    // address. **Not whether there is an internet**: the bus is on the LAN
    // (§10.3), so `wifimgr`'s ping check has no vote here, and a household
    // router with no uplink is a perfectly good place to run this device.
    void OnNetwork(bool up, uint32_t now_ms);
    bool Network() const { return network_; }

    // Try at the next opportunity, ignoring the backoff — the console's
    // `nats connect`. It still needs an address and a network: forcing the
    // question conjures neither.
    void ConnectNow(uint32_t now_ms);

    // Whatever is up is now wrong — a changed address, or `nats retry`. From
    // idle it is `ConnectNow`; from connected it is a disconnect followed
    // immediately by an attempt at the new place.
    void Restart(uint32_t now_ms);

    // The pump. One outstanding attempt at a time, always.
    Action Tick(uint32_t now_ms);

    // The answer to the attempt `Tick` last asked for. A result nobody asked
    // for is ignored — the rule `reachability.h` and `sync_policy.h` both
    // state, and for the same reason: a late answer must not move a machine
    // that has already moved on.
    void OnResult(bool ok, uint32_t now_ms);

    // The socket went away without being asked to. Ignored unless something
    // was actually up.
    void OnDropped(uint32_t now_ms);

    State CurrentState() const;
    bool Connected() const { return connected_; }

    // `kNever` when nothing is scheduled — off, no network, or already
    // connected. Zero when an attempt is due or outstanding.
    uint32_t NextAttemptInMs(uint32_t now_ms) const;

    // How long the connection that is up has been up. Zero when none is.
    uint32_t ConnectedForMs(uint32_t now_ms) const;

    uint16_t Connects() const { return connects_; }
    uint16_t Drops() const { return drops_; }

    // Consecutive refusals, cleared by one success: "it has never worked" and
    // "it missed the last one" look identical from the state alone.
    uint16_t Failures() const { return failures_; }

   private:
    // Wrap-safe throughout: `now_ms` is `esp_timer` milliseconds and rolls
    // over every ~49 days, so every deadline is a signed difference and never
    // a `now >= deadline`.
    static bool Reached(uint32_t now_ms, uint32_t at_ms) {
        return static_cast<int32_t>(now_ms - at_ms) >= 0;
    }

    LinkSettings settings_ = {};
    bool network_ = false;
    bool connected_ = false;
    bool awaiting_ = false;
    bool restart_ = false;

    uint32_t next_at_ms_ = 0;
    uint32_t connected_at_ms_ = 0;
    uint32_t backoff_ms_ = 2000;

    uint16_t connects_ = 0;
    uint16_t drops_ = 0;
    uint16_t failures_ = 0;
};

}  // namespace nats
