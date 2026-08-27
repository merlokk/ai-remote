#pragma once

// The configuration web server (CLAUDE.md §10.16) — `esp_http_server` over the
// pages in SPIFFS, started and stopped on demand.
//
// **It is the sibling board's server, and the reason it exists here is not the
// sibling board's reason.** There, §10.8.6 arrived at an on-screen keyboard 6 mm
// wide and named a phone's keyboard as the affordable alternative. Here there is
// no keyboard at all and no glass to put one on: the only ways into this device
// were the CH343P console and a `config.json` written by a reflash. So on this
// board the site is not a nicer way to type an SSID — it is **the only way that
// does not need a cable**, which is also why `auto` is exactly the right default
// (up while this device is its own access point, which is when somebody has no
// other way in).
//
// What it is *not*, and each of these is a decision rather than an omission:
//
//   * **it can never reach a verdict** (§10.10, rule 4). It serves files, reports
//     state, and writes two sections of `config.json`; the only path to `allow` is
//     a fingertip on a security key, and nothing here can ask for one. The
//     whitelist of `web_settings.h` is where that is enforced by name — `approval`
//     and `led` are refused, so neither the gate's timeout nor what the light says
//     is reachable from a network;
//   * **it does not serve `fido.json`, and cannot.** The enrolment sits on the
//     same filesystem as the pages (§10.18), and `web_paths.h`'s whitelist of
//     *extensions* is what makes that unrepresentable rather than remembered;
//   * **it is asked for rather than started.** The operator sets a *desired*
//     state — off, on, or auto — and the server comes up only when there is a
//     network stack to come up in, which is `web_policy.h`'s three-line rule and
//     the thing that replaced a panic on the sibling board (§10.16). `auto` is
//     §10.9's fallback access point case: up while this device is its own AP.
//
// One thing about it that breaks a house rule on purpose: **the server's task
// stack is not static.** §10.14.1 binds our code, not the libraries' — lwIP,
// Wi-Fi and the USB Host Library all allocate — and `esp_http_server` takes its
// stack from the heap with no static option. That is precisely why the leak
// question is worth asking rather than assuming, and why `web` prints the heap
// either side.
//
// **And the heap it is measured against is the internal one**, the same call
// `status` on the console makes and for its reason: this board has 8 MB of PSRAM
// (§10.13), so `esp_get_free_heap_size` would report eight and a half megabytes
// and say nothing about the memory that is actually scarce. Task stacks, `.bss`
// and anything a driver wants during an interrupt come out of internal RAM. The
// PSRAM's own free space travels in `/api/status` on its own field, the way the
// console gives it its own line.

#include <cstdint>

#include "esp_err.h"
#include "web_policy.h"

namespace web {

// Port 80: this is a device on a desk being configured from a phone, and a port
// number in a URL is one more thing to get wrong.
inline constexpr uint16_t kPort = 80;

// **Three, against `HTTPD_DEFAULT_CONFIG`'s seven**, and it is the sibling
// board's number kept for a weaker reason. There, each open socket could queue
// `CONFIG_LWIP_TCP_SND_BUF_DEFAULT` bytes on its way out against about 18 KB of
// free internal RAM, and the arithmetic mattered: a browser asking for a page and
// its script at once was a request that never finished. Here there are eight
// megabytes of PSRAM behind lwIP's pools and no 48 KB graphics pool competing for
// the internal heap, so the ceiling is *not* load-bearing on this board.
//
// It is kept at three anyway, and that is deliberate rather than inherited: the
// pages are the same pages, written to be fetched one at a time (§10.16), so a
// higher number would buy concurrency nothing asks for. `lru_purge_enable` below
// is what makes a fourth request close the oldest rather than fail.
inline constexpr uint16_t kMaxSockets = 3;

// **8 KB, and the 4 KB this was ported with rebooted the board.** This is the one
// number in this component that is not the sibling board's, and the reason is
// `/api/devstatus`: it runs the console's whole dump on *this* task, and on this
// device that dump includes `key` and `keys` (§10.18) — readouts the console
// itself gives `kConsoleStackBytes` of 10 KB for. The sibling's dump is I²C reads
// and formatting and left 1,756 bytes of 4,096; **this one left 116.**
//
// 116 bytes is not a margin, and the way it failed is worth writing down because
// it is the reason a passing request proves nothing here: the dump *succeeded*,
// 3,339 bytes of it, and then the board rebooted several requests later. A stack
// overflow on FreeRTOS is caught by a canary checked at a **context switch**, not
// at the write that caused it — so the corruption and the panic are separated by
// however long the task keeps running. Three attempts to reproduce it from the
// payloads that appeared to trigger it all passed, which is exactly what a
// delayed-detection overflow looks like from the outside.
//
// So: **8,192, against a measured peak of 3,980** — a little over 2× the deepest
// path this task has. It is not 10 KB like the console because the one thing that
// makes *that* number necessary is not reachable from here: `key selftest` runs
// two curve operations, and nothing on this server can ask for one (§10.10 rule 4).
// `Status::task_stack_low` is the number to check after any change to a readout,
// and `web` on the console prints it against this constant.
//
// It costs internal RAM while the server is up and nothing when it is down — a
// task stack may not live in PSRAM (§10.13), so this is 4 KB more of the scarce
// pool for as long as somebody is configuring the device.
inline constexpr uint32_t kTaskStackBytes = 8192;

// **1 — below everything, including the light.** The sibling board put this below
// its screen task at 3; the equivalent sentence here is that a page is never more
// urgent than the one output this device has. `led` and `indicator` are at 2, the
// responder, the gate, the Wi-Fi manager and the bus are at 4, and the USB host's
// two tasks are at 5 (§10.18.3) — so a browser cannot delay a colour change, and
// cannot come between a touch and a signature.
inline constexpr int kTaskPriority = 1;

// What a file is sent in. In `.bss` rather than on a stack (§10.14.1).
//
// **512 rather than 1,024**, because on this device the interesting number is not
// throughput: a page is a few kilobytes and the wire is a LAN.
inline constexpr uint32_t kChunkBytes = 512;

// Everything `web` on the console prints, taken at one instant.
struct Status {
    // **Desired and actual, side by side**, which is §10.9's shape: the operator
    // asks for off / on / auto, and this says what is really up. A readout that
    // showed only one of the two would make a server that is waiting for a network
    // look like a broken one.
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

    // **The measurement**, in internal RAM (the header says why). Free heap just
    // before `httpd_start`, free heap now, and the low-water mark — which is the
    // number §10.14.1 says decides whether a device is safe. `after_stop` is what
    // answers "does it give it all back": the free heap the last `Stop` left.
    uint32_t free_before_start = 0;
    uint32_t free_now = 0;
    uint32_t low_water = 0;
    uint32_t free_after_stop = 0;

    // The server task's own margin, sampled after the heaviest handler there is
    // (`/api/devstatus` formats every console readout on this board). Zero until
    // something has been served.
    uint32_t task_stack_low = 0;

    // **What the last stop gave back, computed where both halves belong to the
    // same run.** The console used to subtract these two itself and got nonsense
    // the moment anything else moved the heap in between — a radio switched off
    // between a start and a stop releases tens of kilobytes, and the readout
    // reported the server as having "cost" minus forty-two kilobytes. A pair of
    // numbers is only a measurement while nothing else is measured with it.
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
// the way `wifimgr::Apply` is. `main`'s `SettingsChanged` is where it is called.
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
// pointer, the way `indicator` reaches every subsystem through `main`'s gatherer.
//
// **It must be called with no `wifimgr` lock held**: it asks `wifimgr::Get()` for
// the link state, and that takes the same non-recursive mutex the manager's own
// pump holds. `wifi_manager.cpp` already invokes the hook outside it and says so
// where it does — including *why* it is called before the pump rather than after.
void Maintain();

// --- What `/api/devstatus` prints ----------------------------------------
//
// **A hook rather than a call**: the dump belongs to the console, which already
// owns every readout in it (§10.7), and this component must not depend on the
// console — `cli` already depends on *this* one, and a cycle in `REQUIRES` is a
// build nobody enjoys.
//
// So `console::Init` hands its own `PrintDevStatus` over, and the endpoint answers
// `503` until somebody does. That function was already exported before this
// component existed, against exactly this day — `console.h` says so.
using DiagnosticsPrinter = void (*)();
void SetDiagnostics(DiagnosticsPrinter printer);

// --- What `/api/status` says about the approval loop -----------------------
//
// The numbers the front page is actually for: could this device answer a request
// right now, and what has it answered. They belong to `components/responder`, and
// **this component asks `main` for them rather than asking it directly** — which
// on the sibling board was forced by a dependency cycle through `screens` and here
// is a choice: `responder` requires `fido`, and a page that could reach the
// responder would be one refactor away from being able to reach the gate. The
// dependency runs the way `indicator`'s does — `main` hands over a function that
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

    // **The fourth outcome, and this board is the reason it is a field.** §10.10
    // rule 2 says a timeout is not a deny and the two are counted apart; on the
    // sibling board a press is cheap, so the interesting number was allow-versus-
    // deny. Here every verdict costs a touch, so "nobody touched the key" is the
    // ordinary third answer and a page that summed it into `denied` would be
    // reporting refusals nobody made. This is `responder::Status::gate_declined`.
    uint32_t declined = 0;

    // What the key is doing, which is the one health question this device has that
    // the sibling board does not (§10.18). Not "is it working" — that is
    // `key selftest` and it costs a curve operation — but the two facts a page can
    // have for free: something is plugged into the OTG port, and this firmware
    // holds an enrolment derived from one.
    bool key_present = false;
    bool key_enrolled = false;

    // And what the one output is saying right now, in `indicator::StateName`'s own
    // words (§10.17). It is here rather than read from `indicator` directly for the
    // reason the counters are: one probe, filled by the one file that may depend on
    // everything.
    //
    // **It is a readout and never an instruction.** A page that could set this
    // would be a page that can make the device lie about what it did, which is the
    // sentence §10.17 compiles the palette in for.
    const char *light = "";
};
using ApprovalsProbe = void (*)(Approvals *out);
void SetApprovals(ApprovalsProbe probe);

// Starts it. Refused with `ESP_ERR_INVALID_STATE` when it is already up, when
// storage is not mounted — a server with no pages to serve is a port open for
// nothing — and when there is no network stack, which is the refusal that
// replaced a panic (§10.16).
esp_err_t Start();

// Stops it, and gives the task and its stack back to the heap. Idempotent —
// and it *fails* rather than pretending: `httpd_stop` returns without doing
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
