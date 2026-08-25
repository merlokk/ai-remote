#pragma once

// The thing that owns the bus connection, and the only place the socket and
// the policy meet (CLAUDE.md §10.3, §10.5, §10.9).
//
// It is `wifimgr` again, one layer up: `nats_bus.h` is the driver — four verbs
// over a socket, no opinions — `link_policy.h` is the decisions, and this is
// the task that turns one into the other, reads the address out of
// `config.json`, and keeps a snapshot for the console. The split is what makes
// the policy testable on the host without a board (§10.11).
//
// **What it waits for is a network, not an internet.** §10.9 says only
// `ONLINE` releases the bus task, and on this device "online" means a client
// link with an address: the server is on the LAN (§10.3), so the ping check's
// verdict about 8.8.8.8 has no vote here. A household router with its uplink
// down is a perfectly good place to approve a command.
//
// The radio being an access point is not a network for this purpose either —
// there is no route to the bus from a device that *is* the network.
//
// It is the logic layer rather than the library layer (§10.14.2): it reads
// `config.json` and knows what "the configured server" means. What it still
// does not know is anything about approvals — no `key_id`, no `behavior`.
// §10.13's build order puts the responder on top of this, not inside it.

#include <cstddef>
#include <cstdint>

#include "endpoint.h"
#include "esp_err.h"
#include "link_policy.h"
#include "nats_bus.h"

namespace nats {

// How much stack the bus task gets. In the header because the console prints
// the low-water mark against it, and two copies of a number like this one
// drift the first time it is raised — which, given how it was chosen, it may
// well be. `nats_link.cpp` has the story.
inline constexpr uint32_t kTaskStackBytes = 8192;

// Everything a console line — and, later, §10.8.1's link indicator — needs,
// taken at one instant. The task keeps it up to date; nothing here reaches
// into the client, so reading it can never wait behind a connect.
struct Status {
    bool ready = false;  // the task started

    // **What was asked for, and what is happening**, side by side, for the
    // reason §10.9 gives: a readout that showed only the second would make a
    // device that is trying look like a device that is broken.
    bool wanted = false;      // the console's switch, and a URL to go with it
    bool configured = false;  // `nats.url` parsed into something reachable
    Endpoint endpoint = {};

    State state = State::kOff;
    bool network = false;

    uint32_t next_attempt_ms = kNever;
    uint32_t connected_for_ms = 0;

    uint16_t connects = 0;
    uint16_t drops = 0;
    uint16_t failures = 0;

    // Why the last attempt failed. `ESP_OK` before the first one.
    esp_err_t last_error = ESP_OK;

    Counters counters;
    size_t subscriptions = 0;

    // The **lowest** the task's free stack has ever been, in bytes — the same
    // call §10.14.1 makes about the heap: the minimum ever seen is the number
    // that says whether the device is safe, and the current value says nothing.
    // It is here because the number it is watching was chosen after a stack
    // overflow rather than before one.
    uint32_t stack_low_water = 0;
};

// Starts the task. Opens no socket: what happens next is whatever
// `config.json` asks for, and a device with no address configured pays a task
// and nothing else.
esp_err_t Init();
bool Ready();

// Re-read `nats.url`. A changed address drops the connection that is up —
// it is to the wrong server — and reconnects at once; an unchanged one costs
// nothing, which is the §10.9 lesson about narrow settings calls applied here
// before it can be learned the hard way again.
void Apply();

// The console's on/off, and an override that does not touch the file — the
// same split `config set` makes (§10.15): this changes what the device is
// doing, `config save` is what makes it survive a reboot. There is no
// `nats.active` in the file on purpose: an empty `nats.url` is already the one
// switch that means "there is nothing to connect to".
void SetDesired(bool on);
bool Desired();

// Try now rather than at the end of the backoff — the console's
// `nats connect`. Needs an address and a network like everything else.
void ConnectNow();

// Drop what is up and start again from the top, whether or not anything is
// wrong with it — `nats retry`.
void Restart();

Status Get();

// §10.5's surface, for whatever ends up speaking §7. Each one is the `Bus`
// method with the lifetime taken care of: the task can be tearing the client
// down in the same breath, and these are the calls that make that safe.
esp_err_t Publish(const char *subject, const char *payload, const char *reply);
esp_err_t Subscribe(const char *subject, const char *queue, Handler handler, void *user);
esp_err_t Unsubscribe(const char *subject);
bool Flush(uint32_t timeout_ms);

// One subscription, **copied out** rather than pointed at: `Bus` hands back
// pointers into its own table, and the task can free that table between the
// read and the `printf` that uses it.
struct SubscriptionRow {
    char subject[kSubjectSize];
    char queue[kSubjectSize];
    int sid;
};

// For a readout, and bounded by `kMaxSubscriptions`.
bool SubscriptionAt(size_t index, SubscriptionRow *out);

}  // namespace nats
