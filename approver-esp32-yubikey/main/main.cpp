// The entry point. `main/` stays thin (CLAUDE.md §10.14.2): the library layer
// lives in `components/` and the logic goes on top of it, and what this file does
// is **compose**, in an order that is written down rather than implied.
//
// What it composes: the filesystem, the settings on it, the identity, the board
// — which on this device is one button and one LED — the console, the radio, the
// bus, the security key on the OTG port, and the responder that ties them
// together. Every step below says why it is
// where it is; the one rule none of them breaks is that a failure here is a log
// line and not a branch (§10.10: a device that cannot mount its storage should
// still come up far enough to say so).
//
// **And it is the one file that knows every subsystem**, which is what makes it
// the right place for the two hooks at the bottom: the gatherer that tells the
// light what the device is (§10.17), and the list of who to re-tell when
// `config.json` changes under them.

#include <cinttypes>

#include "board.h"
#include "config.h"
#include "console.h"
#include "crypto.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "fido.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "indicator.h"
#include "led.h"
#include "nats_link.h"
#include "registrar.h"
#include "responder.h"
#include "storage.h"
#include "web_server.h"
#include "wifi_manager.h"

namespace {

constexpr const char *TAG = "app";

// **`BOOT` is read once, at boot, and only if it is already down** (§10.15).
//
// The sibling board reads its restore button *through the reset*, blind, for five
// seconds. This one cannot: `BOOT` is GPIO0 and GPIO0 held across a reset is the
// ROM's download strap, so a restore asked for that way would be a restore nobody
// could perform.
//
// So the rule here is: the operator presses BOOT *while the board is coming up*,
// and keeps holding. If the pin is high when `app_main` looks, nothing happens and
// the boot costs one GPIO read — which is every ordinary boot. If it is low, the
// LED goes white and the five-second hold of `config::kRestoreHoldMs` begins, with
// feedback the C6 board's five silent seconds never had.
// Set while that hold is being counted, so the light can say so (§10.17 — white,
// solid, the brightest thing this device does).
volatile bool g_restore_window = false;

// Set once `app_main` has composed everything. Until then the light says
// `kBooting` — red, solid — which is the state the repository owner asked for by
// name and which doubles as proof that the emitter, the UART and the encoding all
// work before anything else has had a chance to go wrong.
volatile bool g_booted = false;

// --- What the light is told (§10.17) --------------------------------------
//
// **The one function in this firmware that is allowed to ask every subsystem how
// it is.** `components/indicator` has never heard of a radio, a bus or a key; it
// takes a struct of booleans and ranks them. This is where that struct is filled,
// and it is here rather than there for the same reason `config::OnChanged`'s list
// is here: `main` is the only file that may depend on everything.
void GatherState(indicator::Inputs *out) {
    out->booting = !g_booted;
    out->restore_window = g_restore_window;

    out->storage_mounted = storage::Mounted();
    out->can_verify = crypto::Ready();
    out->registered = registration::Registered();

    const wifimgr::Snapshot wifi = wifimgr::Get();
    out->wifi_link = wifi.radio.mode == wifi::Mode::kClient &&
                     wifi.radio.link == wifi::Link::kConnected && wifi.radio.ip != 0;
    // **The reachability answer, not the association.** §10.9's whole argument:
    // a router with no uplink looks like a healthy connection from the station's
    // side, and the two states have separate fixes and separate colours.
    //
    // With the check switched off there is nothing to disagree with, so a link is
    // taken at its word — otherwise a device with `internet.check` false would
    // spend its life one rung below where it actually is.
    out->internet = config::Get().internet.check
                        ? wifi.internet != wifimgr::Internet::kOffline
                        : out->wifi_link;

    const nats::Status bus = nats::Get();
    out->bus_connected = bus.state == nats::State::kConnected;

    const responder::Status approvals = responder::Get();
    out->subscribed = approvals.subscribed;
    out->request_pending = responder::RequestPending();
    out->signing = responder::Busy();
    out->deny_pending = responder::DenyPending();

    out->fido_present = fido::Present();
    out->fido_enrolled = fido::Enrolled();

    // **What counts as a fault, and it is deliberately short.** Not "something is
    // missing" — the ranking below already has a colour for each of those — but
    // "something that should work did not". libsodium failing its own vector is the
    // one that matters: §6's reply could not be checked, so this device cannot
    // safely learn whose handler it is talking to — and it will not register at all
    // (`responder::Blocker::kCannotVerify`).
    out->fault = !crypto::Ready() && storage::Mounted() && config::Loaded();
}

// --- What the configuration page says about the approval loop -------------
//
// **`components/web` may not ask `components/responder` for this**, and on this
// board that is a choice rather than a forced one (`web/CMakeLists.txt` argues
// it): `responder` requires `fido`, and a component that could reach the
// responder would be one refactor away from being able to reach the *gate* —
// which is the one thing §10.10 rule 4 says nothing on a network may touch. So
// `main` is the one place that sees both, which is where it belongs anyway
// (§10.14.2) — the same shape as `indicator::OnGather` above and
// `web::SetDiagnostics` next door.
//
// Three groups, and each is a question the front page asks: could this device
// answer a request, what has it answered, and what is it saying about itself.
void FillApprovals(web::Approvals *out) {
    const responder::Status now = responder::Get();
    out->ready = now.ready;
    out->subscribed = now.subscribed;
    out->blocked_by = responder::BlockerText(now.blocked_by);
    out->received = now.received;
    out->allowed = now.allowed;
    out->denied = now.denied;
    // **Kept apart from `denied`, because §10.10 rule 2 says they are different
    // outcomes.** On this board that distinction is the ordinary case rather than
    // an edge: every verdict costs a touch, so "nobody touched the key" is what
    // most unanswered requests are, and a page that summed this into `denied`
    // would be reporting refusals nobody made.
    out->declined = now.gate_declined;
    out->replied = now.replied;

    // The two facts about the key a page can have for free (§10.18). Not "does it
    // work" — that is `key selftest`, it costs two curve operations, and it is a
    // command somebody types rather than something a browser triggers.
    out->key_present = fido::Present();
    out->key_enrolled = fido::Enrolled();

    // And what the one output is saying, in the ranking's own words (§10.17), so
    // that a page and the light on the desk cannot disagree about what this device
    // is. **A readout and never an instruction**: there is no setter beside it,
    // for the reason §10.17 compiles the palette in.
    out->light = indicator::StateName(indicator::Current());
}

// --- Who is holding a copy of a field in `config.json` --------------------
//
// Called once at boot and again on every `config reload` / `config restore`,
// through `config::OnChanged` — which is the hook that exists so that the three
// callers of a reload do not each keep their own list of who to tell.
//
// Each of these is the narrowest call that re-reads what changed. §10.9's lesson
// from `wifi check`, which used to reconnect a working link in order to change a
// ping list.
void SettingsChanged() {
    led::SetBrightness(config::Get().led.percent, config::Get().led.idle_percent);
    wifimgr::Apply();
    nats::Apply();
    // **And the configuration site's own mode** (§10.16), which is why a `config
    // reload` that says `web.mode: off` can take the page down that asked for the
    // reload. `web_server.cpp`'s action handler says so where it offers the button.
    web::Apply();
    // The light itself, so that a brightness change or a fresh enrolment is
    // visible on the next frame rather than at the next state change.
    indicator::Poke();
}

// The restore window of §10.15, run on the main task before the rest of `app_main`
// gets going. Returns what the button asked for.
bool WaitForRestoreHold() {
    if (!board::BootPressed()) {
        // Nobody is holding anything, which is every ordinary boot. One GPIO read
        // and the window never opens — the alternative would be ten seconds of
        // white LED on a device that just wanted to start.
        return false;
    }

    g_restore_window = true;
    indicator::Poke();
    ESP_LOGW(TAG, "BOOT is down; hold it for %" PRIu32 " ms to restore %s",
             config::kRestoreHoldMs, config::kPath);

    const bool held =
        board::Buttons().HeldFor(board::button::kBootIndex, config::kRestoreHoldMs);

    g_restore_window = false;
    indicator::Poke();
    return held;
}

}  // namespace

extern "C" void app_main(void) {
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_app_desc_t *desc = esp_app_get_description();

    ESP_LOGI(TAG, "%s %s, running from %s at 0x%" PRIx32 " (%" PRIu32 " KB)", desc->project_name,
             desc->version, running->label, running->address, running->size / 1024);

    // **The LED first, and on this board that is not a stylistic choice.** It is
    // the only output the device has, so every line below can be reported and none
    // of them can be reported before this one runs. It depends on nothing — a
    // UART and a pin — and the first thing it does is go red (§10.17), which is
    // the operator's proof that the firmware is running at all.
    //
    // `board::Init` is what brings up both it and the button; the indicator task
    // that follows is what puts a colour on it.
    board::Init();
    indicator::Init();
    indicator::OnGather(GatherState);

    // Order matters and is written down rather than implied (§10.14.1), and it
    // reads bottom-up: the filesystem, then the settings on it, then the identity,
    // then the console — so that `cat`, `keys` and `config` all have something to
    // answer with the moment the prompt appears. None of these failures is fatal.
    storage::Init();

    // **`BOOT` after boot, and it is here rather than three lines lower because
    // that is the whole point** (§10.15): the failure this button exists for is a
    // `config.json` that stops the device booting, so the restore has to run
    // *before* the parse that would fall over.
    //
    // Unlike the sibling board's five blind seconds, this hold has feedback — the
    // LED is already up and goes white solid for the length of it. That is the one
    // thing this board's single button buys back for not being readable at reset.
    config::RestoreAtBoot(WaitForRestoreHold());

    config::Init();
    // The operator's brightness, before anything else has a chance to be seen at
    // the compiled-in default.
    led::SetBrightness(config::Get().led.percent, config::Get().led.idle_percent);

    // **The verifier — and there is no identity here any more** (§10.6). This used
    // to derive an Ed25519 key for this board to sign with; §10.18 moved the signer
    // into the security key, so what this call now does is bring libsodium up, check
    // it against a vector Python produced, and **erase the seed an older firmware
    // left in NVS**. That erase is the point: it takes the last private key off this
    // board's flash.
    //
    // Still this early, and for a reason that survived the change: it depends on
    // nothing — no filesystem, no radio — and everything above the bus has to be
    // able to ask whether §6's reply can be checked before it registers.
    //
    // The failure path is silent by design (§10.10): a self-test that fails leaves
    // `crypto::Ready()` false, and `responder::WhyNot` will not let the device onto
    // `approvals.*` without it. A board that cannot verify the handler cannot know
    // whose key it pinned.
    crypto::Init();

    // And what the key is *for*: `registration.json`, if there is one (§10.7). It
    // reads a file and speaks to nobody, so it belongs here next to the key — and a
    // device that is not registered is not an error state, it is the state a freshly
    // flashed board is in.
    registration::Init();

    console::Init();

    // The radio (§10.9), after the settings it reads and after the console that can
    // fix them. It starts a task and **not** the radio: what happens next is
    // whatever `config.json` asks for, and the shipped file asks for nothing —
    // `esp_wifi_init` costs tens of kilobytes of heap, and a device configured with
    // Wi-Fi off should not pay them.
    wifimgr::Init();

    // **The configuration web server (§10.16), which starts nothing here.** It
    // reads `web.mode` and registers itself on the manager's tick; whether it
    // actually comes up is `web::ShouldRun`'s answer, and on a device whose radio
    // is off the answer is no — `httpd_start` without lwIP is a panic, not an
    // error, and that is the whole reason this is a wish rather than a call.
    //
    // After `wifimgr::Init` and not before it, because the tick it borrows is that
    // manager's: there is no task here to own the server's lifetime, which is
    // 2.5 KB of permanent RAM not spent on managing a 7 KB on-demand cost.
    //
    // **On this board it is the only way in that needs no cable** (`web_server.h`):
    // there is no glass and no keyboard, so the alternative to this page is the
    // CH343P console. `console::Init` above has already handed over the dump the
    // site serves at `/api/devstatus` — one printer, two surfaces (§10.7).
    web::Init();
    web::SetApprovals(FillApprovals);

    // And the bus (§10.3), after the radio for the same reason: it has nothing to
    // do until there is a client link with an address.
    //
    // **It waits for a *network*, not for an internet**, because the server is on
    // the LAN and a household router with its uplink down is a perfectly good place
    // to approve a command. There is nothing else here that wants the internet:
    // this device has no clock to sync (§10.13), which is the one subsystem on the
    // sibling board that did.
    nats::Init();

    // **The security key on the OTG port** (§10.18). It brings up the USB Host
    // Library and loads `fido.json` if there is one; it plugs into nothing and asks
    // nothing until a request arrives.
    //
    // After the bus rather than before it for one reason: the Host Library's daemon
    // task and the Wi-Fi driver both want heap at start-up, and the one that can be
    // done without is this one. A device whose USB host will not start is a device
    // that cannot approve, which is the safe direction (§10.10) — but it should
    // still be a device that can be talked to.
    const esp_err_t fido_err = fido::Init();
    if (fido_err != ESP_OK) {
        ESP_LOGE(TAG, "the key gate is not available: %s", esp_err_to_name(fido_err));
    }

    // **Now that there is an enrolled key to compare against** (§10.18.1). This is
    // deliberately not inside `registration::Init()`, which runs long before the
    // enrolment is loaded and would call every registration stale — it did, and the
    // line it printed was an instruction to spend a one-time token for nothing.
    registration::ReportKeyBinding();

    // Settings applied to the tasks holding copies of them. `main` is where the
    // hook is registered for the same reason the gatherer is: it is the one file
    // that may depend on everything.
    //
    // **The same function is what a reload calls**, so a `config reload` typed on
    // the console and a restore re-apply one list rather than two that drift.
    config::OnChanged(SettingsChanged);

    // **Last.** It subscribes to nothing yet — it wants a key, a registration and a
    // bus first, and `request` on the console says which of the three is missing.
    //
    // It is the last line of `app_main` for the reason §10.14.1 gives about the main
    // task's 8 KB stack: everything is composed by now, ESP-IDF deletes this task
    // when this function returns, and its stack goes back to the heap with it.
    const esp_err_t responder_err = responder::Init();
    if (responder_err != ESP_OK) {
        ESP_LOGE(TAG, "responder not started: %s", esp_err_to_name(responder_err));
    }

    // And the light stops saying `booting`. Everything below this line in time is
    // the device doing its job; everything above it was composition, and the red
    // that was on the emitter for it is the only state that cannot be wrong.
    g_booted = true;
    indicator::Poke();

    const char *restored = config::BootRestoreText();
    if (restored != nullptr) {
        ESP_LOGW(TAG, "%s", restored);
    }
}
