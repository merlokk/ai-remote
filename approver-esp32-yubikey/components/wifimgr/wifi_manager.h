#pragma once

// The thing that owns the radio and the policy, and the only place the two
// meet (CLAUDE.md §10.9).
//
// Everything interesting is next door in `wifi_policy.h` — this is the plumbing
// around it, and the list is short because that is the point of the split:
//
//   * one task, polling at `kPollMs`, that turns what `wifi::Radio` reports
//     into policy events and what the policy asks for into radio calls;
//   * the settings, read out of `config.json` (§10.15) — which is also where
//     the SSIDs and the passwords live, so that the policy can stay a state
//     machine that has never seen a credential;
//   * a snapshot for the console (§10.7) and, later, for §10.8.6's screen.
//
// It is the logic layer, not the library layer (§10.14.2): it reads
// `config.json` and knows what a "remembered network" is. `wifi::Radio` below
// it knows neither. What it deliberately does **not** know is anything about
// approvals — no `key_id`, no bus. The NATS client is a separate tenant of
// "online" and will wait on it the way §10.9 says: only `ONLINE` releases the
// bus task.
//
// **The radio is brought up lazily.** `esp_wifi_init` costs tens of kilobytes
// of heap, the shipped `config.json` has `wifi.active` false, and a device
// configured with the radio off should pay nothing for the fact that this
// component exists.

#include <cstddef>
#include <cstdint>

#include "esp_err.h"
#include "reachability.h"
#include "wifi_policy.h"
#include "wifi_radio.h"

namespace wifimgr {

// How often the manager task looks at the radio and pumps the policy. Fast
// enough that a press on the settings screen feels immediate, slow enough to
// be free: 200 ms of a single core is five wakeups a second doing almost
// nothing.
inline constexpr uint32_t kPollMs = 200;

// Everything a screen or a console line needs, taken at one instant. The
// desired state and the current one, side by side, because §10.9's whole shape
// is the difference between them.
struct Snapshot {
    Desired desired = Desired::kOff;
    State state = State::kOff;
    Failure failure = Failure::kNone;

    uint8_t network = kNoNetwork;  // index into `config.wifi.networks`
    uint8_t round = 0;
    bool auth_failed[kMaxNetworks] = {};

    uint32_t ap_window_remaining_ms = 0;  // `kHeldOpen` when it does not run
    uint32_t wait_remaining_ms = 0;

    // What the radio itself says. The two can disagree for a moment — the
    // policy has asked for something the driver has not finished doing — and
    // showing both is how that reads as a device working rather than a device
    // confused.
    wifi::Status radio;

    bool radio_ready = false;

    // **Whether the TCP/IP stack exists**, which is a different question from
    // whether the radio is ready and a much sharper one for anything that opens a
    // socket: §10.9 brings `esp_netif_init` and the netifs up lazily, inside the
    // first `StartClient` / `StartAp` / `Scan`. Before that, lwIP's mailboxes do
    // not exist — and a `listen()` against them is an `assert` inside
    // `tcpip_send_msg_wait_sem`, not an error code. §10.16's server is the first
    // thing in this firmware that could reach it, and this is what it asks.
    bool stack_up = false;

    esp_err_t radio_error = ESP_OK;

    // **Whether anything is reachable through the link**, which is a separate
    // question from whether there is one (§10.9). `kUnknown` while there is no
    // client link, while the check is switched off, and until the first round
    // has answered — a device that reported "offline" because it had not asked
    // yet would be lying.
    Internet internet = Internet::kUnknown;
    uint8_t internet_failed_rounds = 0;
    uint32_t internet_last_ok_ms = kNeverSucceeded;  // age, not a timestamp
    uint32_t internet_next_probe_ms = 0;
    uint8_t internet_target = kNoTarget;
};

// Starts the manager task. Does **not** touch the radio: what happens next is
// whatever `config.json` asks for, applied by the task.
esp_err_t Init();
bool Ready();

// Re-reads `config.json` into the policy and restarts whatever was in
// progress — **including dropping a working link**, because the network list
// it was working from may have just changed. Called after `config reload`,
// `config restore` and any edit to the networks.
void Apply();

// The internet check alone, and **nothing about which network to be on**.
// Editing the ping list through `Apply` cost a perfectly good connection and a
// walk back through the whole list: the targets have nothing to do with the
// radio, and a settings call that reconnects is a settings call people stop
// making.
void ApplyInternetCheck();

// An override that does not touch the file — the same split `config set` makes
// (§10.15): this changes what the device is doing, `config save` is what makes
// it survive a reboot.
void SetDesired(Desired desired);

Snapshot Get();

// --- Somebody else's tick (§10.16) ---------------------------------------
//
// **A hook, so that this manager can be the clock for something without knowing
// what it is.** The configuration web server has to come up when there is a
// network and go down when there is not, and it has nothing at all to do in
// between — the same argument anything that only has to notice a state change
// makes for having no task of its own. This task already polls the radio five
// times a second; lending that tick out costs a function pointer, and a task of
// its own would have cost 2.5 KB of permanent RAM.
//
// The dependency runs one way, like `screens::OnDecision`: `wifimgr` holds a
// pointer and never learns what is on the other end of it.
//
// **It is called with no lock of this manager held**, deliberately: the handler
// is expected to call `Get()`, and that takes the same non-recursive mutex the
// pump holds. One handler; setting a second replaces the first.
using TickHandler = void (*)(void *user);
void OnTick(TickHandler handler, void *user);

// Ask about the internet now instead of at the next interval — the console's
// `wifi ping`. Does nothing when there is no client link to ask through.
//
// **Nothing on this board acts on the answer except the light** (§10.17): the
// sibling firmware also had a clock that waited for an internet before asking a
// time server, and there is no clock here (§10.13). Anything that grows one later
// should read `Snapshot::internet` rather than add a second probe of its own.
void CheckInternetNow();

// Straight through to the driver, and blocking for a second or two. Never
// called from the manager task, and §10.8.6's rule stands: only when somebody
// is looking at a list, never on a timer.
//
// **This is the one call that will start a radio nobody asked for**, and put
// it back afterwards: "what is out there" is the question you have precisely
// when the radio is off and nothing works. The lazy bring-up above is spent
// the first time it is asked.
esp_err_t Scan(wifi::ScanResult *out, size_t capacity, size_t *found);

// The desired mode `config.json` currently asks for. `wifi.active` false is
// `kOff` whatever `wifi.mode` says — one switch that means "radio down",
// rather than two fields that can disagree.
Desired DesiredFromConfig();

}  // namespace wifimgr
