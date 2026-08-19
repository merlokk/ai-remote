#pragma once

// **When** the configuration web server should be up (CLAUDE.md §10.16).
//
// `<cstdint>` and nothing else, which is the shape the Wi-Fi policy, the sync
// schedule, the bus link and every screen's arithmetic already have (§10.11) —
// and here it buys the same thing: the one rule that keeps this server from
// crashing the device is a comparison, and a comparison can be tested.
//
// The rule is the repository owner's, and it is the same split §10.9 draws
// between what was *asked for* and what is *happening*: the operator asks for
// off, on or auto, and the server actually comes up only when there is a TCP/IP
// stack for it to live in. §10.16 records why that is not a nicety —
// `httpd_start` with lwIP down is an `assert` and a reboot, not an error code.

#include <cstdint>

namespace web {

// What the operator asked for. Not what is running.
enum class Desired : uint8_t {
    kOff = 0,  // never
    kOn,       // whenever there is a network to serve on
    // **Only while this device is an access point** — its own AP by request, or
    // §10.9's fallback one that goes up when nothing else would have it. That is
    // precisely the case the server exists for: a device that cannot reach a
    // network, and an operator who has no other way in. On a working client link
    // it stays down, and the 7 KB stays free.
    kAuto,
};

const char *Name(Desired desired);

// The whole decision, and it is deliberately four arguments rather than a peek at
// the world:
//
//   * `network_wanted` — whether the radio is *supposed* to be up at all. Not
//     whether it is: this is what lets the server let go **before** the manager
//     takes the network away, and §10.16 records what the other order costs (a
//     null call inside `esp_netif_free_rx_buffer` and a reboot, because closing a
//     socket needs the netif that queued its packets to still exist);
//   * `stack_up` — `wifimgr::Snapshot::stack_up`, whether `esp_netif_init` has
//     happened at all. It gates every mode, `kOn` included: a server started with
//     no lwIP is not a server, it is an assert;
//   * `ap` — whether this device is currently being an access point, which is the
//     one thing `kAuto` waits for.
bool ShouldRun(Desired desired, bool network_wanted, bool stack_up, bool ap);

}  // namespace web
