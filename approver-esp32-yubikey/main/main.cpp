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
#include "device_key.h"
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
    out->device_key = crypto::Ready();
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

    out->fido_required = config::Get().approval.require_key;
    out->fido_present = fido::Present();
    out->fido_enrolled = fido::Enrolled();

    // **What counts as a fault, and it is deliberately short.** Not "something is
    // missing" — the ranking below already has a colour for each of those — but
    // "something that should work did not". A key that fails its self-test is the
    // one that matters: a device that cannot sign will take requests off the queue
    // group and answer none of them.
    out->fault = !crypto::Ready() && storage::Mounted() && config::Loaded();
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
    // The light itself, so that a brightness change or a `requireKey` flip is
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

    if (!config::Get().approval.require_key) {
        // Said out loud, once, at boot. A device that can approve without a key
        // must never be one somebody forgot they configured (§10.18).
        ESP_LOGW(TAG, "approval.requireKey is FALSE - this device will approve on a button alone");
    }

    // **The identity, and it is this early because everything above the bus has to
    // be able to ask about it** (§10.6). It depends on nothing — no filesystem, no
    // radio — so the first console prompt finds the answer already there, and a
    // device that cannot sign says so from its first log line rather than from its
    // first request.
    //
    // The failure paths are silent by design (§10.10): a self-test that fails or an
    // eFuse with no key burned leaves `crypto::Ready()` false, and nothing above may
    // publish a decision without it.
    //
    // **This is also why the main task's stack is 8 KB** — `crypto_sign` uses 4,112
    // bytes of it and the framework's default is 3,584. `sdkconfig.defaults` carries
    // that number and where it came from.
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
