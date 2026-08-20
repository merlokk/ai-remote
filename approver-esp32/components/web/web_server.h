#pragma once

// The configuration web server (CLAUDE.md §10.16) — `esp_http_server` over the
// pages in SPIFFS, started and stopped on demand.
//
// **It exists to be measured before it is trusted.** §10.16 asks two questions
// that only a board can answer: what does it cost while it is up, and does
// stopping it give every byte back. So this component is deliberately the
// smallest thing that can answer them — static files and one read-only JSON
// endpoint — and the console drives it (`web on` / `web off` / `web cycle`).
//
// What it is *not*, yet, and each of these is a decision rather than an omission:
//
//   * **nothing here writes a setting.** No form, no config endpoint. A writable
//     config over HTTP is a way to point this device at another NATS server, and
//     that needs the authentication question answered first — §10.16 has it, and
//     the answer this repository is likely to want is "bind to the access-point
//     interface only". **One POST exists**, and it is the exception the repository
//     owner asked for: `/api/reboot`, confirmed by a word in the query
//     (`web_paths.h`). It changes no state that survives it — which is the whole
//     of why it was allowed in ahead of the rest;
//   * **it is asked for rather than started.** The operator sets a *desired*
//     state — off, on, or auto — and the server comes up only when there is a
//     network stack to come up in, which is `web_policy.h`'s three-line rule and
//     the thing that replaced a panic. `auto` is §10.9's fallback access point
//     case: up while this device is its own AP, which is exactly when somebody
//     needs a way in and has no other;
//   * **it can never reach a verdict** (§10.10). It serves files and reports
//     state; the only path to `allow` is a human press on a card.
//
// One thing about it that breaks a house rule on purpose: **the server's task
// stack is not static.** §10.14.1 binds our code, not the libraries' — lwIP,
// Wi-Fi and mbedTLS all allocate — and `esp_http_server` takes its stack from the
// heap with no static option. That is precisely why the leak question is worth
// asking rather than assuming, and why `web` prints the heap either side.

#include <cstdint>

#include "esp_err.h"
#include "web_policy.h"

namespace web {

// Port 80: this is a device on a desk being configured from a phone, and a port
// number in a URL is one more thing to get wrong.
inline constexpr uint16_t kPort = 80;

// **Two, against `HTTPD_DEFAULT_CONFIG`'s seven**, and the arithmetic is the
// reason: each open socket is a `sock_db` plus an lwIP PCB, and each *active* one
// can queue up to `CONFIG_LWIP_TCP_SND_BUF_DEFAULT` bytes on its way out. That
// number is 2,880 in `sdkconfig.defaults` and was 5,760 — the framework's
// default — until a page hung.
//
// **It was three, and the board is what changed it.** Three sockets at 5,760 is
// 17 KB of possible in-flight data on a device with about 18 KB free while the
// server is up, and a browser asking for a page and its two files at once is
// exactly that: the symptom is a request that never finishes and about 5 KB that
// does not come back. Two sockets at 2,880 is under 6 KB, and `lru_purge_enable`
// below is what makes a third request close the oldest rather than fail (§10.16
// has the numbers either side).
inline constexpr uint16_t kMaxSockets = 3;

// The default 4 KB, kept — the house firmware of §10.14.4 raises it to 8 because
// its handlers put a 2,500-byte buffer on that stack, and ours puts its buffer in
// `.bss` instead (§10.14.1).
inline constexpr uint32_t kTaskStackBytes = 4096;

// **Below the screen task (3), not `HTTPD_DEFAULT_CONFIG`'s 5.** At 5 it would
// preempt LVGL (4) and the screens (3) to serve a page — and a page is never more
// urgent than the press on a permission request that this device exists for
// (§10.8.1).
inline constexpr int kTaskPriority = 2;

// What a file is sent in. In `.bss` rather than on a stack, which is where the
// house firmware of §10.14.4 puts its 2,500 (§10.14.1).
//
// **512 rather than 1,024**, because on this device the interesting number is not
// throughput: a page is a few kilobytes and the wire is a LAN. Half a kilobyte is
// half a kilobyte of `.bss` given back to the heap the Wi-Fi driver allocates its
// TX buffers from, and §10.16 records what happens when that heap runs out.
inline constexpr uint32_t kChunkBytes = 512;

// Everything `web` on the console prints, taken at one instant.
struct Status {
    // **Desired and actual, side by side**, which is §10.9's shape and the
    // repository owner's request: the operator asks for off / on / auto, and this
    // says what is really up. A readout that showed only one of the two would
    // make a server that is waiting for a network look like a broken one.
    Desired desired = Desired::kAuto;
    bool running = false;
    uint16_t port = 0;

    uint32_t requests = 0;   // answered with a file
    uint32_t api = 0;        // answered with the JSON
    uint32_t refused = 0;    // the whitelist said no (§10.16: `config.json`)
    uint32_t not_found = 0;  // an allowed name that is not on the filesystem
    uint32_t bytes = 0;      // sent out of files

    // **Whether a credential is being asked for, and how often one was missing or
    // wrong** (§10.16, `web_auth.h`). Both belong in the readout for the same
    // reason: a site whose config has half a credential in it is *open*, and an
    // operator has to be able to see that rather than assume the opposite. A
    // climbing count with `auth` true is somebody guessing; with `auth` false it
    // cannot happen at all.
    bool auth = false;
    uint32_t unauthorised = 0;

    // **The measurement.** Free heap just before `httpd_start`, free heap now,
    // and the low-water mark — which is the number §10.14.1 says decides whether
    // a device is safe. `after_stop` is what answers "does it give it all back":
    // the free heap the last `Stop` left behind.
    uint32_t free_before_start = 0;
    uint32_t free_now = 0;
    uint32_t low_water = 0;
    uint32_t free_after_stop = 0;

    // The server task's own margin, sampled after the heaviest handler there is
    // (`/api/devstatus` reads the I²C bus and formats a few hundred lines). Zero
    // until something has been served.
    uint32_t task_stack_low = 0;

    // **What the last stop gave back, computed where both halves belong to the
    // same run.** The console used to subtract these two itself and got nonsense
    // the moment anything else moved the heap in between — a radio switched off
    // between a start and a stop releases 41 KB, and the readout reported the
    // server as having "cost" minus forty-two kilobytes. A pair of numbers is only
    // a measurement while nothing else is measured with it.
    int32_t last_run_returned = 0;
    bool has_run = false;
};

// --- The desired state ---------------------------------------------------
//
// Registers the reconciler and reads `web.mode` out of `config.json`. Called by
// `main` after `wifimgr::Init`, because the reconciler runs on that manager's
// task — see `Maintain` below.
esp_err_t Init();

// What the operator asked for. **In memory only**, the same split every other
// setter makes (§10.15): this changes what the device is doing, `config save` is
// what makes it survive a reboot.
void SetDesired(Desired desired);
Desired GetDesired();

// What `config.json` currently asks for, which is a different question from what
// is *wanted* — the console can set a mode without saving it, the same split
// §10.15 makes everywhere. `web` prints both.
Desired DesiredFromConfig();

// Re-read `web.mode` from the config — after `config reload` / `config restore`,
// the way `wifimgr::Apply` is.
void Apply();

// **The reconcile: desired against what is possible.** Starts the server when
// `web::ShouldRun` says it should be up and stops it when it should not, so a
// radio that goes down takes the server with it rather than leaving a socket
// nobody can reach.
//
// It runs on the **Wi-Fi manager's task**, which is a deliberate choice and the
// one thing about this component that needs stating twice: that task already
// polls the radio five times a second, so the server needs no task and no stack
// of its own — which would have been 2.5 KB of permanent RAM to manage a 7 KB
// on-demand cost. `wifimgr` knows nothing about this: it calls a function
// pointer, the way `screens` reaches the responder (§10.8.4).
//
// **It must be called with no `wifimgr` lock held**: it asks `wifimgr::Get()` for
// the link state, and that takes the same non-recursive mutex the manager's own
// pump holds. The hook is invoked outside it, and `wifi_manager.cpp` says so
// where it does that.
void Maintain();

// --- What `/api/devstatus` prints ----------------------------------------
//
// **A hook rather than a call, for the third time in this firmware** (after
// `screens::OnDecision` and `wifimgr::OnTick`): the dump belongs to the console,
// which already owns every readout in it (§10.7), and this component must not
// depend on the console — `cli` already depends on *this* one, and a cycle in
// `REQUIRES` is a build nobody enjoys.
//
// So `console::Init` hands its own `PrintDevStatus` over, and the endpoint answers
// `503` until somebody does. The printer writes with `printf`; the plumbing that
// turns that into a response is `web_server.cpp`'s, and it is the one interesting
// thing in this component.
using DiagnosticsPrinter = void (*)();
void SetDiagnostics(DiagnosticsPrinter printer);

// --- What `/api/status` says about the approval loop -----------------------
//
// The numbers the front page is actually for: could this device answer a request
// right now, and what has it answered. They belong to `components/responder`, and
// **this component may not ask it** — `responder` requires `screens`, `screens`
// requires this one for the row that says whether the server is up (§10.8.5), and
// a cycle in `REQUIRES` is a build that does not happen. So the dependency runs
// the way the other four in this firmware do: `main` hands over a function that
// fills this in, and the fields simply read zero on a device where nobody did.
//
// A copy of the fields rather than a pointer to somebody's struct, for the reason
// §10.9 gives about handing the radio a copy of an address: what is on the other
// end of this can change between two reads, and a document is one instant.
struct Approvals {
    bool ready = false;
    bool subscribed = false;
    const char *blocked_by = "";

    uint32_t received = 0;
    uint32_t allowed = 0;
    uint32_t denied = 0;
    uint32_t replied = 0;
};
using ApprovalsProbe = void (*)(Approvals *out);
void SetApprovals(ApprovalsProbe probe);

// Starts it. Refused with `ESP_ERR_INVALID_STATE` when it is already up, when
// storage is not mounted — a server with no pages to serve is a port open for
// nothing — and when there is no network stack, which is the refusal that
// replaced a panic (§10.16).
esp_err_t Start();

// Stops it, and gives the task and its stack back to the heap. Idempotent —
// and it now *fails* rather than pretending: `httpd_stop` returns without doing
// anything if it cannot signal its own task, and a handle dropped after that is
// a running server nobody can reach again (§10.16).
esp_err_t Stop();

// **Take the server's lifetime away from the reconciler**, for as long as a
// caller means to drive `Start`/`Stop` itself — which today is exactly one
// caller, the `web cycle` diagnostic. While it is held, `Maintain()` does
// nothing at all whatever the radio does.
//
// It is not a lock and it does not block: it is the answer to "who owns this",
// and the lock underneath is the component's own. Release it on every path out —
// a hold left set is a server the network can no longer bring up or take down.
// `web_policy.h` argues why this exists, and §10.16 has the double free that
// bought it.
void Hold(bool held);

bool Running();

Status Get();

}  // namespace web
