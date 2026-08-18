#include "screens.h"

#include "config.h"

#include <cinttypes>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "clock_face.h"
#include "clock_screen.h"
#include "idle_policy.h"
#include "limits_screen.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "lvgl_display.h"
#include "nats_link.h"
#include "navigator.h"
#include "request_card.h"
#include "request_screen.h"
#include "settings_screen.h"
#include "status_screen.h"
#include "touch_screen.h"
#include "wifi_manager.h"

namespace screens {
namespace {

constexpr const char *TAG = "screens";

// Static, like every task in this firmware (§10.14.1): the stack is an array and
// the TCB is a struct, so nothing here depends on the heap being in a good mood.
StackType_t g_stack[kTaskStackBytes / sizeof(StackType_t)];
StaticTask_t g_task = {};
TaskHandle_t g_handle = nullptr;

ClockScreen g_clock;
ui::ClockFace g_face;
RequestScreen g_request_screen;
ui::RequestCard g_card;

// The limits of §10.8.3, which arrive rather than being navigated to —
// `ui/limits_view.h` says why and at whose request.
LimitsScreen g_limits_screen;
ui::LimitsView g_limits;

// Settings and the status pages (§10.8.5). Same split as everything else here:
// `ui::SettingsMenu` and `ui::StatusPager` decide and are host-tested, the two
// screens paint and decide nothing.
SettingsScreen g_settings_screen;
ui::SettingsMenu g_menu;
StatusScreen g_status_screen;
ui::StatusPager g_pager;

// The touch test and the calibration (§10.8.5). **The one screen that reads the
// panel itself**: everything else takes its touch through LVGL, and this has to
// see the raw point because it is measuring the correction rather than using it.
TouchScreen g_touch_screen;
ui::TouchFlow g_touch_flow;
display::Touch *g_touch = nullptr;

// Where the finger was and how long it has been there, kept between passes so
// that a release can be turned into a point. Only meaningful while that screen
// is up; `Reset` on the flow is what clears the rest.
bool g_touch_down = false;
uint32_t g_touch_down_ms = 0;
uint16_t g_touch_last_raw_x = 0;
uint16_t g_touch_last_raw_y = 0;

// **`KEY` held two seconds opens settings**, which is the operator's way in when
// the panel is not being touched — and the reason the threshold and the
// short/long split live in `buttons.h` rather than here is §10.11: this file
// cannot be tested and that one can.
constexpr uint32_t kSettingsHoldMs = 2000;
buttons::PressLength g_key_hold(kSettingsHoldMs);

// What a finger drew across the glass, recorded by an LVGL event callback and
// read by the task. Both run under the display lock — the callback because
// `lv_timer_handler` holds it while it dispatches, the task because it takes it
// to paint — so the handoff needs no lock of its own, which is the same argument
// the two screens' `TakeTap` makes.
lv_dir_t g_gesture = LV_DIR_NONE;

// Asked for from somewhere that is not a finger: the console (`screen`). It goes
// through the same door a gesture does and is consumed on the next pass.
ui::Nav g_pending_nav = ui::Nav::kBack;
bool g_has_pending_nav = false;

// **The navigator is kept in step with the card rather than owning the queue.**
// §10.8.1 gives it the rule that navigation vanishes while a card is up, and it
// deliberately does not hold the requests themselves ("a navigator that stored
// `tool_name` would be a navigator with the protocol in it"). So the card queue
// is where they live and this is told when one arrives, is answered or expires —
// the count in each is the same count, stepped in one place.
ui::Navigator g_nav;

Hardware g_hardware;
pmic::Axp2101 *g_battery = nullptr;
Keys g_keys;

// --- The idle timer (§10.8.1) --------------------------------------------
//
// **The rule §10.8.1 states and nothing kept**: brightness drops on an idle
// timeout, the panel blanks after it, and anything at all wakes it. The two
// settings that were supposed to carry it were read by nobody at all;
// `ui/idle_policy.h` is where the decisions moved to and where the argument
// lives. What is here is the three things that cannot be host-tested — reading
// the world, and telling the panel.
ui::IdlePolicy g_idle;
display::Panel *g_panel = nullptr;

// What the last pass saw, so a finger is one comparison rather than a second
// poll of the controller under the same lease.
uint32_t g_touch_activity_seen = 0;

// The accelerometer, for the two facts §10.8.1 needs from it: is the board being
// moved, and is it standing on its USB edge. Read at its own slow rate — the
// clock's battery is 2 s and this is 500 ms, because a device picked up should
// light before it reaches eye level.
constexpr uint32_t kMotionEveryTicks = 25;
static_assert(kMotionEveryTicks % kFaceEveryTicks == 0,
              "the motion read has to land on a pass that is not skipped");
float g_last_accel[3] = {0.0f, 0.0f, 0.0f};
bool g_have_accel = false;

// Set when the settings changed under the policy, so that a `config set
// brightness` is on the glass before the operator has finished reading the line
// that confirmed it. A state change alone would not do it: the state did not
// change, the level it means did.
bool g_display_dirty = true;

// Something happened. Safe from any task — `ui::IdlePolicy::Activity` is one
// store, and it is deliberately not the thing that recomputes anything.
void Activity() { g_idle.Activity(static_cast<uint32_t>(esp_timer_get_time() / 1000)); }

// **Where a decided request is handed back, and it is not the stack.** A
// `ui::Request` is 2.3 KB of §7 fields; two of them as locals in the button poll
// took this task's free stack from 2,944 bytes to 1,088, which is not a margin.
// Static, used only by the screen task, and §10.14.1 would have asked for this
// anyway — the measurement is just what made it urgent.
ui::Request g_answered;

// **What the receipt says under an answered card**, and it is set from outside
// because only the thing that publishes knows whether anything did. The default
// is the honest one for a device with nothing behind the screen: `main` wires the
// responder up, and until it does, a press decides and nothing leaves.
//
// §10.10's "failure is visible", in the smallest place it appears: a receipt that
// implied a reply had gone is the one lie on this screen that would matter.
constexpr size_t kNoteSize = 48;

// The notice under the date. Short: it is a headline, not a sentence.
constexpr size_t kNoticeSize = 32;
char g_note[kNoteSize] = "decided, not sent - nothing is listening";

// §10.15's "say it happened": one line under the date, set by `main` once there
// is a screen, and taken away by `ui::ClockFace` a minute later. Empty is the
// ordinary state — nothing to say — and there is no lock around it because it
// is written once, before anything reads it.
char g_notice[kNoticeSize] = {};
uint32_t g_notice_since_ms = 0;
bool g_notice_set = false;

// Who to tell. Null until somebody asks for it, which is the state a device with
// no responder is in — and it is not an error, it is `request test` with nobody
// behind it.
DecisionHandler g_handler = nullptr;
void *g_handler_user = nullptr;

// **The chirp of §10.8.1, on a task of its own.** `PlayWav` blocks for the length
// of the file — `alert.wav` is about three and a half seconds — and there is no
// task in this firmware that can afford that: the screen task would miss the
// press it exists to see, the bus task would stop reading the socket, and the
// responder task would hold up the signature. So it gets 4 KB and a semaphore,
// and the worst a stalled chirp can do is delay the next chirp.
//
// **Binary on purpose**: four cards arriving together are one noise. §10.8.1 asks
// for one short sound on a new request, not a queue of them.
audio::Speaker *g_alert = nullptr;
StackType_t g_alert_stack[4096 / sizeof(StackType_t)];
StaticTask_t g_alert_tcb;
StaticSemaphore_t g_alert_signal_storage;
SemaphoreHandle_t g_alert_signal = nullptr;

void AlertTask(void *) {
    for (;;) {
        xSemaphoreTake(g_alert_signal, portMAX_DELAY);
        if (g_alert != nullptr && g_alert->Ready()) {
            g_alert->PlayWav(kAlertSound);
        }
    }
}

// The snapshot the console reads, and a lock around it. Short critical sections
// only, and **never held across the display lock**: the two would then have an
// order to get wrong, and a console command waiting behind a frame is a console
// that looks hung.
Status g_status;
SemaphoreHandle_t g_lock = nullptr;
StaticSemaphore_t g_lock_storage;

// §10.9 has six states and §10.8.2's icon has four shapes. The mapping is the
// interesting part, and the two collapses in it are deliberate:
//
//   * the fallback access point and a permanent one are **one glyph**. §10.9
//     keeps them apart because one is an answer and the other a symptom, and
//     that difference belongs to a settings screen — on a 40-pixel icon it would
//     be a distinction nobody could see;
//   * `kWaiting` is `connecting`. A backoff is an attempt that has not started
//     yet, and an icon that went blank between tries would flicker between "off"
//     and "trying" for as long as a network was refusing us.
ui::WifiIcon MapWifi(const wifimgr::Snapshot &wifi) {
    switch (wifi.state) {
        case wifimgr::State::kOnline:
            return ui::WifiIcon::kClient;
        case wifimgr::State::kConnecting:
        case wifimgr::State::kWaiting:
            return ui::WifiIcon::kConnecting;
        case wifimgr::State::kAp:
        case wifimgr::State::kApWindow:
            return ui::WifiIcon::kAp;
        case wifimgr::State::kOff:
            break;
    }
    return ui::WifiIcon::kOff;
}

void Gather(ui::ClockInputs *in, bool *battery_valid, pmic::Status *battery) {
    in->now_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000);

    // The clock is UTC and a zone is presentation (§10.8.2): the epoch decides
    // whether the time is believable and `localtime_r` decides what it says.
    const time_t now = std::time(nullptr);
    in->epoch_utc = static_cast<int64_t>(now);
    std::tm local = {};
    localtime_r(&now, &local);
    in->hour = static_cast<uint8_t>(local.tm_hour);
    in->minute = static_cast<uint8_t>(local.tm_min);

    const wifimgr::Snapshot wifi = wifimgr::Get();
    in->wifi = MapWifi(wifi);
    in->rssi = wifi.radio.rssi;

    // **What the dot is about is the socket, not the internet** — the bus is on
    // the LAN (§10.3), so `wifimgr`'s ping verdict has no vote here, exactly as
    // it has none in `nats_link.cpp`.
    const nats::Status bus = nats::Get();
    in->bus_configured = bus.configured;
    in->bus_connected = bus.state == nats::State::kConnected;
    in->bus_messages = bus.counters.messages_in;

    in->notice = g_notice_set;
    in->notice_since_ms = g_notice_since_ms;

    if (*battery_valid) {
        in->battery_present = battery->battery_present;
        in->vbus_present = battery->vbus_present;
        in->charging = battery->charging;
        in->battery_percent = static_cast<int16_t>(battery->battery_percent);
    } else {
        in->battery_present = false;
        in->vbus_present = false;
        in->charging = false;
        in->battery_percent = -1;
    }
}

// --- The status pages (§10.8.5) ------------------------------------------
//
// The one place in this component that gathers rather than draws, which is the
// job §10.8.1 leaves it: an I²C read never happens inside an LVGL callback, and
// this runs on the screen task with that lock nowhere in sight.
//
// Every value is bounded by `kStatusValueSize` and none of them may run off the
// right margin — a number read wrong is worse than a number missing, which is
// the call §10.8.4 makes about a truncated command for much higher stakes.

const char *ResetReasonName(esp_reset_reason_t reason) {
    switch (reason) {
        case ESP_RST_POWERON:
            return "power on";
        case ESP_RST_EXT:
            return "reset pin";
        case ESP_RST_SW:
            return "software";
        case ESP_RST_PANIC:
            return "panic";
        case ESP_RST_INT_WDT:
            return "interrupt wdt";
        case ESP_RST_TASK_WDT:
            return "task wdt";
        case ESP_RST_WDT:
            return "other wdt";
        case ESP_RST_DEEPSLEEP:
            return "deep sleep";
        case ESP_RST_BROWNOUT:
            return "brownout";
        case ESP_RST_SDIO:
            return "sdio";
        case ESP_RST_USB:
            return "usb";
        case ESP_RST_JTAG:
            return "jtag";
        default:
            break;
    }
    return "unknown";
}

void Row(StatusFacts *facts, const char *label, const char *format, ...) {
    if (facts->rows >= kStatusRows) {
        return;
    }
    facts->label[facts->rows] = label;
    va_list args;
    va_start(args, format);
    vsnprintf(facts->value[facts->rows], kStatusValueSize, format, args);
    va_end(args);
    ++facts->rows;
}

void FillPower(StatusFacts *facts, bool battery_valid, const pmic::Status &battery) {
    facts->title = "power";
    if (!battery_valid) {
        Row(facts, "pmic", "not answering");
        return;
    }

    if (battery.battery_present && battery.battery_percent >= 0) {
        Row(facts, "battery", "%d%%, %u.%02u V", battery.battery_percent,
            battery.battery_mv / 1000U, (battery.battery_mv % 1000U) / 10U);
    } else if (battery.battery_present) {
        Row(facts, "battery", "%u.%02u V", battery.battery_mv / 1000U,
            (battery.battery_mv % 1000U) / 10U);
    } else {
        // §10.8.2's rule about a state that is not a fault: no cell on the
        // connector is a board on a cable, not a battery at zero.
        Row(facts, "battery", "none, on the cable");
    }

    const char *state = "idle";
    if (battery.charging) {
        state = "charging";
    } else if (battery.discharging) {
        state = "discharging";
    } else if (!battery.battery_present) {
        state = "no cell";
    }
    Row(facts, "charge", "%s", state);

    if (battery.vbus_present) {
        Row(facts, "usb", "in, %u.%02u V", battery.vbus_mv / 1000U,
            (battery.vbus_mv % 1000U) / 10U);
    } else {
        Row(facts, "usb", "out");
    }
    Row(facts, "system", "%u.%02u V", battery.system_mv / 1000U,
        (battery.system_mv % 1000U) / 10U);
    Row(facts, "aldo2", "%u.%02u V %s", battery.aldo2_mv / 1000U,
        (battery.aldo2_mv % 1000U) / 10U, battery.aldo2_enabled ? "on" : "off");
    Row(facts, "aldo3", "%u.%02u V %s", battery.aldo3_mv / 1000U,
        (battery.aldo3_mv % 1000U) / 10U, battery.aldo3_enabled ? "on" : "off");
    Row(facts, "die", "%.1f C", static_cast<double>(battery.die_celsius));
    // Why the board is awake at all — the chip's own answer (§10.1), which is a
    // different question from why the firmware last restarted, one page along.
    Row(facts, "awake", "%s", pmic::PowerOnSourceName(battery.power_on_source));
}

void FillSystem(StatusFacts *facts) {
    facts->title = "system";

    Row(facts, "restart", "%s", ResetReasonName(esp_reset_reason()));

    const uint32_t seconds = static_cast<uint32_t>(esp_timer_get_time() / 1000000);
    Row(facts, "uptime", "%ud %02uh %02um %02us", seconds / 86400U, (seconds % 86400U) / 3600U,
        (seconds % 3600U) / 60U, seconds % 60U);

    Row(facts, "heap", "%u free", static_cast<unsigned>(esp_get_free_heap_size()));
    // **The low-water mark rather than the current free heap** (§10.14.1): the
    // minimum ever seen is the number that says whether the device is safe.
    Row(facts, "lowest", "%u",
        static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_DEFAULT)));

    const esp_app_desc_t *desc = esp_app_get_description();
    Row(facts, "firmware", "%s", desc != nullptr ? desc->version : "?");

    const wifimgr::Snapshot wifi = wifimgr::Get();
    if (wifi.radio.ssid[0] != 0) {
        Row(facts, "wifi", "%s %s", wifimgr::Name(wifi.state), wifi.radio.ssid);
    } else {
        Row(facts, "wifi", "%s", wifimgr::Name(wifi.state));
    }
    if (wifi.radio.link == wifi::Link::kConnected) {
        Row(facts, "signal", "%d dBm, ch %u", static_cast<int>(wifi.radio.rssi),
            static_cast<unsigned>(wifi.radio.channel));
        Row(facts, "ip", "%u.%u.%u.%u", static_cast<unsigned>(wifi.radio.ip & 0xFF),
            static_cast<unsigned>((wifi.radio.ip >> 8) & 0xFF),
            static_cast<unsigned>((wifi.radio.ip >> 16) & 0xFF),
            static_cast<unsigned>((wifi.radio.ip >> 24) & 0xFF));
    }

    const nats::Status bus = nats::Get();
    const char *bus_text = "no server set";
    if (bus.configured) {
        bus_text = bus.state == nats::State::kConnected ? "connected" : "down";
    }
    Row(facts, "bus", "%s", bus_text);
}

void FillMotion(StatusFacts *facts) {
    facts->title = "motion";
    ::imu::Qmi8658 *motion = g_hardware.motion;
    if (motion == nullptr || !motion->Present()) {
        Row(facts, "imu", "not present");
        return;
    }

    ::imu::Sample sample = {};
    if (motion->Read(&sample) != ESP_OK) {
        // A lease it could not get, or a chip that stopped answering. Saying so
        // beats six zeros that read as a board lying perfectly flat.
        Row(facts, "imu", "no reading");
        return;
    }

    Row(facts, "accel x", "%+.3f g", static_cast<double>(sample.accel_g[0]));
    Row(facts, "accel y", "%+.3f g", static_cast<double>(sample.accel_g[1]));
    Row(facts, "accel z", "%+.3f g", static_cast<double>(sample.accel_g[2]));
    // The one line that says the other three mean anything: at rest the vector is
    // 1 g, and a range bit that did not take gives six believable numbers and the
    // wrong magnitude (§10.7).
    // `total` rather than `magnitude`, which is nine characters and one too many
    // for the label column — see `status_screen.cpp`. Eight is the budget.
    Row(facts, "total", "%.3f g of 1.000",
        static_cast<double>(::imu::Qmi8658::Magnitude(sample)));
    Row(facts, "gyro x", "%+.1f dps", static_cast<double>(sample.gyro_dps[0]));
    Row(facts, "gyro y", "%+.1f dps", static_cast<double>(sample.gyro_dps[1]));
    Row(facts, "gyro z", "%+.1f dps", static_cast<double>(sample.gyro_dps[2]));
    Row(facts, "die", "%.1f C", static_cast<double>(sample.celsius));
}

void FillStatus(StatusFacts *facts, bool battery_valid, const pmic::Status &battery) {
    switch (g_pager.Page()) {
        case ui::StatusPage::kPower:
            FillPower(facts, battery_valid, battery);
            break;
        case ui::StatusPage::kSystem:
            FillSystem(facts);
            break;
        case ui::StatusPage::kMotion:
            FillMotion(facts);
            break;
        case ui::StatusPage::kCount:
            break;
    }
}

// --- The touch test (§10.8.5) --------------------------------------------
//
// **Polled here rather than taken from LVGL**, and that is the point of the
// screen: LVGL's pointer has already been through the correction, so a
// calibration built from it would be measuring its own last answer. This reads
// the controller directly — under the same lease every other reader takes
// (§10.14.3) — and the two readers cost one extra transaction per pass while
// this screen is up and nothing at all when it is not.
void PollTouch(TouchView *view, uint32_t now_ms) {
    uint16_t raw_x = 0;
    uint16_t raw_y = 0;
    bool down = false;

    if (g_touch != nullptr && g_touch->Ready()) {
        down = g_touch->ReadRaw(&raw_x, &raw_y);
    }

    if (down && g_touch != nullptr) {
        if (!g_touch_down) {
            g_touch_down = true;
            g_touch_down_ms = now_ms;
        }
        g_touch_last_raw_x = raw_x;
        g_touch_last_raw_y = raw_y;
    } else if (g_touch_down && g_touch != nullptr) {
        // **The point is taken on release, at where the finger last was.** A
        // press is what the operator can still adjust; a release is what they
        // meant. Taking it on the way down would record the first frame of a
        // finger still landing.
        g_touch_down = false;
        const uint32_t held = now_ms - g_touch_down_ms;
        if (g_touch_flow.Released(static_cast<int16_t>(g_touch_last_raw_x),
                                  static_cast<int16_t>(g_touch_last_raw_y), held) &&
            g_touch_flow.Collected() >= ui::kTouchTargets) {
            ui::TouchCalibration fitted = g_touch->Calibration();
            const ui::TouchFit outcome =
                g_touch_flow.Finish(kPanelWidth, kPanelHeight, &fitted, now_ms);
            if (outcome == ui::TouchFit::kOk) {
                // Applied at once and **written by nobody**: `config set` and
                // `config save` are two commands for the reason §10.15 gives,
                // and a screen that reached the filesystem would be the one
                // place in this firmware that did not follow it.
                g_touch->SetCalibration(fitted);
                config::Get().touch.scale_x = fitted.scale_x;
                config::Get().touch.scale_y = fitted.scale_y;
                config::Get().touch.offset_x = fitted.offset_x;
                config::Get().touch.offset_y = fitted.offset_y;
                ESP_LOGI(TAG, "touch calibrated: x %d/1000%+d, y %d/1000%+d - 'config save' keeps it",
                         static_cast<int>(fitted.scale_x), static_cast<int>(fitted.offset_x),
                         static_cast<int>(fitted.scale_y), static_cast<int>(fitted.offset_y));
            } else {
                ESP_LOGW(TAG, "touch calibration refused: %s", ui::TouchFitText(outcome));
            }
        }
    }

    // **Everything the screen shows is read after the point was handled**, so a
    // press that completed a calibration is drawn against the correction it just
    // produced rather than against the one it replaced.
    view->touching = down;
    view->raw_x = raw_x;
    view->raw_y = raw_y;
    view->screen_x = raw_x;
    view->screen_y = raw_y;
    if (g_touch != nullptr) {
        view->calibration = g_touch->Calibration();
    }
    view->calibration.Apply(kPanelWidth, kPanelHeight, &view->screen_x, &view->screen_y);
    view->stage = g_touch_flow.Stage();
    view->collected = g_touch_flow.Collected();
    view->outcome = g_touch_flow.Outcome();
    if (view->stage == ui::TouchStage::kCollecting) {
        ui::TouchTarget(view->collected, kPanelWidth, kPanelHeight, &view->target_x,
                        &view->target_y);
    }
}

// Put the correction back to none. **`KEY` on that screen, and the reason it is
// a button** (`touch_screen.h`): the state this returns to is the one every
// device ships with, so it is always safe, and it must be reachable when the
// glass is not.
void ResetTouchCalibration() {
    if (g_touch == nullptr) {
        return;
    }
    const ui::TouchCalibration none;
    g_touch->SetCalibration(none);
    config::Get().touch.scale_x = none.scale_x;
    config::Get().touch.scale_y = none.scale_y;
    config::Get().touch.offset_x = none.offset_x;
    config::Get().touch.offset_y = none.offset_y;
    g_touch_flow.Reset();
    ESP_LOGI(TAG, "touch correction cleared - 'config save' keeps it");
}

// One decision, handed to whoever registered for it. **This is the seam §10.6 and
// §7 took over**, and the shape of it is the point: nothing about this screen
// changed when they did — no key, no subject and no signature appears in this
// file, and a device with no responder still shows cards and still lets them
// time out.
//
// The log line stays regardless of who is listening, because "somebody pressed
// allow" is a fact worth having in a boot log whether or not it reached a bus.
void Decided(const ui::Request &request, ui::Verdict verdict) {
    const char *word = verdict == ui::Verdict::kAllow ? "ALLOW" : "DENY";
    if (g_handler == nullptr) {
        ESP_LOGW(TAG, "%s %s in %s - nothing published: no responder is wired up", word,
                 request.tool_name, request.cwd);
        return;
    }
    ESP_LOGI(TAG, "%s %s in %s", word, request.tool_name, request.cwd);
    g_handler(request, verdict, g_handler_user);
}

// --- Getting around (§10.8.1, §10.8.5) -----------------------------------
//
// Three ways in and they all end at the same function, which is the point: the
// navigator is the only thing that decides what is on the glass, and a gesture,
// a button and a console command are three ways of asking it the same question.

bool Apply(ui::Nav nav) {
    const ui::ScreenId was = g_nav.Screen();
    const bool leaving_limits = was == ui::ScreenId::kLimits;
    const bool moved = g_nav.Navigate(nav);
    if (!moved) {
        return false;
    }
    if (leaving_limits && nav == ui::Nav::kBack) {
        // §10.8.3: a back out of the limits is also a dismissal, or the numbers
        // arriving every few seconds would put the screen straight back.
        g_limits.Dismissed();
    }
    if (g_nav.Screen() == ui::ScreenId::kSettings) {
        // Arriving at the list clears whatever the last visit left armed and puts
        // the selection at the top — `settings_menu.h` argues why.
        g_menu.Opened();
    }
    if (g_nav.Screen() == ui::ScreenId::kStatus) {
        g_pager.Reset();
    }
    // Arriving *and* leaving both reset the flow: a calibration nobody finished
    // must not be waiting for its third cross the next time this screen opens.
    if (g_nav.Screen() == ui::ScreenId::kTouch || was == ui::ScreenId::kTouch) {
        g_touch_flow.Reset();
        g_touch_down = false;
    }
    return true;
}

// What LVGL says a finger drew. Recorded rather than acted on: this runs inside
// the LVGL task, and §10.8.1 keeps every decision out of there.
void ScreenGesture(lv_event_t *) {
    lv_indev_t *indev = lv_indev_active();
    if (indev != nullptr) {
        g_gesture = lv_indev_get_gesture_dir(indev);
    }
}

ui::Nav NavForGesture(lv_dir_t dir) {
    switch (dir) {
        case LV_DIR_TOP:
            return ui::Nav::kSwipeUp;
        case LV_DIR_LEFT:
            return ui::Nav::kSwipeLeft;
        case LV_DIR_RIGHT:
            return ui::Nav::kSwipeRight;
        case LV_DIR_BOTTOM:
        default:
            break;
    }
    // A swipe down is the way back out of everything a swipe up reached. It is
    // not in the navigator's table as its own action because "back" is already
    // that action, arriving by a different route.
    return ui::Nav::kBack;
}

// **A restart asked for by a finger.** The console's `reboot` argues that no
// confirmation is needed there because a reboot undoes itself in seconds; the
// screen's answer is `settings_menu.h`'s, and by the time this runs the second
// press has already happened. What is left is the part §10.7 found on the board:
// the console is the C6's own USB Serial/JTAG port and it goes down with the
// chip, so the line has to be flushed *and given a moment* or the operator sees
// a console that died rather than one that answered.
void RebootNow() {
    ESP_LOGW(TAG, "reboot from the settings screen");
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(120));
    esp_restart();
}

// **A power-off asked for by a finger**, and the driver is what refuses it while
// the cable is in (§10.1) — the row has already said `usb in` by then, so this
// path is only reached with the cable out. Not a `vTaskDelay` and a flush like
// the reboot above: the console is going down with the chip either way, and the
// PMIC cuts the rails the moment the register is written.
void PowerOffNow() {
    if (g_battery == nullptr || !g_battery->Present()) {
        ESP_LOGW(TAG, "no PMIC, so nothing to switch off");
        return;
    }
    // **And how to come back**, because this is the last thing anybody watching
    // the console will see from this board and the answer is not obvious: a
    // short press on `PWR` powers the chip on (§10.1), and a *long* one is what
    // gets it back if it ever comes up in the ROM's download mode.
    ESP_LOGW(TAG, "power off from the settings screen - a short press on PWR brings it back");
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(120));
    const esp_err_t err = g_battery->PowerOff();
    if (err != ESP_OK) {
        // The one that gets here is `ESP_ERR_INVALID_STATE`: the cable went back
        // in between the row being drawn and the press landing.
        ESP_LOGW(TAG, "not switched off: %s", esp_err_to_name(err));
    }
}

// One row's action, taken on the screen task and never in a callback.
void Activated(uint32_t now_ms) {
    switch (g_menu.Activate(now_ms)) {
        case ui::SettingsAction::kOpenStatus:
            Apply(ui::Nav::kOpenStatus);
            break;
        case ui::SettingsAction::kOpenWifi:
            Apply(ui::Nav::kOpenWifi);
            break;
        case ui::SettingsAction::kReboot:
            RebootNow();
            break;
        case ui::SettingsAction::kPowerOff:
            PowerOffNow();
            break;
        case ui::SettingsAction::kPowerOffBlocked:
            // The row says `usb in` and has said so since before the press, so
            // this is for whoever is reading the log rather than the screen.
            ESP_LOGI(TAG, "not switched off: the cable is in, and it would come straight back on");
            break;
        case ui::SettingsAction::kNotBuilt:
            // The row already says `soon` on the glass, so this is for whoever is
            // watching the log rather than the screen.
            ESP_LOGI(TAG, "that row has no screen behind it yet");
            break;
        case ui::SettingsAction::kOpenTouch:
            Apply(ui::Nav::kOpenTouch);
            break;
        case ui::SettingsAction::kArmed:
        case ui::SettingsAction::kNone:
            break;
    }
}

// The buttons, once per tick. **Edges only** — a level would mean a finger resting
// on the allow button re-approving every card that arrived under it, and
// §10.8.1's rule about a press that began before the card exists to stop exactly
// that. The model is given the same `now_ms` the edge was seen at, which is what
// it compares against the moment the card was presented.
void PollKeys(uint32_t now_ms) {
    if (g_keys.buttons == nullptr || !g_keys.buttons->Ready()) {
        return;
    }

    const bool card_up = g_card.State() == ui::CardState::kCard;
    const ui::ScreenId screen = g_nav.Screen();

    // **All three sampled up front, because an edge on any of them is somebody
    // being here** (§10.8.1's idle timer) — including the ones that go on to do
    // nothing at all. A press the card's guard throws away is still a person
    // pressing a button, and a device that stayed dark through it would be a
    // device you have to press twice.
    const buttons::Event allow = g_keys.buttons->Poll(g_keys.allow);
    const buttons::Event deny = g_keys.buttons->Poll(g_keys.deny);
    const buttons::Event menu_event = g_keys.buttons->Poll(g_keys.menu);
    if (allow != buttons::Event::kNone || deny != buttons::Event::kNone ||
        menu_event != buttons::Event::kNone) {
        g_idle.Activity(now_ms);
    }

    // **And a press that wakes the panel does not also do what it says** — the
    // rule `display::SwallowTouch` keeps for a finger, kept here for the three
    // buttons, because with the display off the operator cannot see what they
    // are about to act on either. `BOOT` would otherwise step the settings list
    // invisibly and `KEY` could reach the reboot row two presses into the dark.
    //
    // It cannot swallow a verdict: a card on the glass holds the screen awake
    // for as long as it is up (§10.10), so `ALLOW` and `DENY` are never in this
    // branch when there is anything to answer. The edges are consumed above, so
    // nothing acts on them late either.
    if (g_idle.State() == ui::DisplayPower::kOff) {
        return;
    }

    if (allow == buttons::Event::kPressed) {
        if (g_card.Press(ui::Verdict::kAllow, now_ms, &g_answered)) {
            g_nav.RequestAnswered();
            Decided(g_answered, ui::Verdict::kAllow);
        } else if (!card_up) {
            // **The same button steps a list when there is no card**, which is
            // the whole of this device's second job: three buttons, and the one
            // that means yes is also the one that means next. It cannot mean both
            // at once, because a card up takes the branch above and §10.8.1 has
            // already taken navigation away.
            if (screen == ui::ScreenId::kSettings) {
                g_menu.Next();
            } else if (screen == ui::ScreenId::kStatus) {
                g_pager.Next();
            } else if (screen == ui::ScreenId::kTouch &&
                       g_touch_flow.Stage() == ui::TouchStage::kTest) {
                g_touch_flow.Start();
            }
        }
    }

    if (deny == buttons::Event::kPressed) {
        if (card_up) {
            // **While a card is up this button is a verdict and nothing else.**
            // §10.8.1: navigation is gone, not deferred — and a press the guard
            // throws away must not fall through to navigating either, or the guard
            // becomes a way to leave the screen instead of a way to protect it.
            if (g_card.Press(ui::Verdict::kDeny, now_ms, &g_answered)) {
                g_nav.RequestAnswered();
                Decided(g_answered, ui::Verdict::kDeny);
            }
        } else {
            // Otherwise it is the way back — one level at a time, and out of the
            // limits screen it is also a dismissal (`Apply` says why).
            Apply(ui::Nav::kBack);
        }
    }

    // **`KEY`: held, it opens the list; tapped, it presses the selected row.**
    // The split is `buttons::PressLength`'s, where it is tested — and the long
    // press fires while the finger is still down, because the operator is holding
    // a button with no feedback but the screen.
    const buttons::Press key = g_key_hold.Update(g_keys.buttons->Pressed(g_keys.menu), now_ms);
    if (card_up) {
        // Nothing at all, and it costs no branch of its own above: the navigator
        // would refuse the long press anyway, and a short one must not press a
        // row on a screen the operator cannot see.
        return;
    }
    if (key == buttons::Press::kLong) {
        Apply(ui::Nav::kSwipeUp);
    } else if (key == buttons::Press::kShort && screen == ui::ScreenId::kSettings) {
        Activated(now_ms);
    } else if (key == buttons::Press::kShort && screen == ui::ScreenId::kTouch) {
        // **The reset, and it is a button because the glass may be the broken
        // thing** (`touch_screen.h`). It puts back the state every device ships
        // with, so it is always safe to press.
        ResetTouchCalibration();
    }
}

// **Telling the panel what the idle timer decided**, and the only place in this
// file that touches it. Under the display lock, because those commands go out on
// the same QSPI wires as the frame LVGL is drawing — the console's
// `display brightness` does it without the lock and gets away with it because a
// human types once a minute; this runs ten times a second.
//
// A lock it could not get is a retry on the next pass rather than a state the
// panel never heard about, which is the rule the battery read next door follows
// for the same reason.
void ApplyDisplayPower() {
    if (g_panel == nullptr || !g_panel->Ready()) {
        return;
    }
    const bool on = g_idle.State() != ui::DisplayPower::kOff;

    display::Lock lock(kLockTimeoutMs);
    if (!lock) {
        g_display_dirty = true;
        return;
    }

    if (on) {
        // Brightness before the panel comes back, or waking from the blank is a
        // flash of whatever level it was at when it went dark.
        g_panel->SetBrightness(g_idle.Brightness());
        if (!g_panel->On()) {
            g_panel->SetOn(true);
        }
    } else {
        g_panel->SetOn(false);
    }

    // §10.8.1's queued touch, in the shape a dark screen has: the finger that
    // brings the panel back must not also press what is under it. Only the blank
    // arms it — a dimmed screen is one the operator can still read.
    display::SwallowTouch(!on);
}

void Publish(const ui::ClockView &view, bool applied) {
    if (xSemaphoreTake(g_lock, pdMS_TO_TICKS(20)) != pdTRUE) {
        return;
    }
    g_status.ready = true;
    g_status.view = view;
    g_status.updates++;
    if (!applied) {
        g_status.lock_misses++;
    }
    g_status.stack_low_water = uxTaskGetStackHighWaterMark(nullptr);

    // The idle timer, so `display` on the console can say why the panel is at
    // the level it is at — the alternative being to sit in front of it for
    // fifteen minutes and see what happens.
    g_status.power = g_idle.State();
    g_status.idle_ms = g_idle.IdleMs(static_cast<uint32_t>(esp_timer_get_time() / 1000));
    g_status.upright = g_idle.Upright();
    xSemaphoreGive(g_lock);
}

void Task(void *) {
    ui::ClockInputs in;
    pmic::Status battery = {};
    bool battery_valid = false;
    uint32_t tick = 0;

    for (;;) {
        const uint32_t now_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000);

        // **The card first, and the buttons before anything that can block.** A
        // press is the one input on this device with a deadline behind it, and
        // everything below — an I2C read, a `strftime`, an LVGL lock — can take
        // milliseconds a finger will not wait for.
        const uint16_t expired_before = g_card.TimedOut();
        const bool card_changed = g_card.Tick(now_ms);
        for (uint16_t i = expired_before; i < g_card.TimedOut(); ++i) {
            g_nav.RequestExpired();
        }
        PollKeys(now_ms);

        // **A finger anywhere on the glass**, taken from the count LVGL's own
        // read callback keeps rather than by polling the controller a second
        // time — that would be another I²C transaction per tick, under the same
        // lease, for a fact somebody already has.
        const uint32_t touches = display::TouchActivity();
        if (touches != g_touch_activity_seen) {
            g_touch_activity_seen = touches;
            g_idle.Activity(now_ms);
        }

        // **A card on the glass holds the screen awake.** §10.10: the one screen
        // this device exists for must not be the one that dims while somebody is
        // reading it, and its arrival being an activity is not enough — a
        // request left unanswered for the length of its TTL would otherwise fade
        // out under the operator.
        if (g_card.State() == ui::CardState::kCard) {
            g_idle.Activity(now_ms);
        }

        // Asked for from the console rather than by a finger. Consumed here so
        // that it goes through exactly the door a gesture does.
        if (g_has_pending_nav) {
            g_has_pending_nav = false;
            Apply(g_pending_nav);
        }

        // Most ticks are a button poll and nothing else. A card that just changed
        // is drawn immediately rather than at the next face pass: an arrival, a
        // press and an expiry are all things the operator is looking at — and so
        // is a fingertip, which is why the touch screen runs at the full rate:
        // a crosshair updated five times a second reads as a device that is not
        // keeping up, on the one screen whose whole job is to look responsive.
        if ((tick % kFaceEveryTicks) != 0 && !card_changed &&
            g_nav.Screen() != ui::ScreenId::kTouch) {
            ++tick;
            vTaskDelay(pdMS_TO_TICKS(kTickMs));
            continue;
        }

        // **A lease it could not get leaves the last reading standing**, which is
        // §10.14.3's own answer for the clock: a charge that is a few seconds old
        // beats an icon that blinks out because the touch controller was busy.
        if (g_battery != nullptr && g_battery->Present() && (tick % kBatteryEveryTicks) == 0) {
            pmic::Status fresh = {};
            if (g_battery->Read(&fresh) == ESP_OK) {
                battery = fresh;
                battery_valid = true;
            }
        }

        // **The accelerometer, for the two facts §10.8.1 needs from it**: is the
        // board being moved, and is it standing on its USB edge. It is the only
        // reader outside the status page, and it changes nothing about §10.13's
        // rule — no gesture approves anything, and what this can do is turn a
        // screen on.
        if (g_hardware.motion != nullptr && g_hardware.motion->Present() &&
            (tick % kMotionEveryTicks) == 0) {
            ::imu::Sample sample = {};
            if (g_hardware.motion->Read(&sample) == ESP_OK) {
                if (g_have_accel && ui::Moved(g_last_accel, sample.accel_g)) {
                    g_idle.Activity(now_ms);
                }
                for (int axis = 0; axis < 3; ++axis) {
                    g_last_accel[axis] = sample.accel_g[axis];
                }
                g_have_accel = true;
                g_idle.SetUpright(ui::StandingButtonsUp(sample.accel_g[0], sample.accel_g[1],
                                                        sample.accel_g[2]));
            }
        }

        // Everything that could have woken it has been read by now, so this is
        // where the panel finds out.
        if (g_idle.Tick(now_ms) || g_display_dirty) {
            g_display_dirty = false;
            ApplyDisplayPower();
        }

        Gather(&in, &battery_valid, &battery);
        const ui::ClockView view = g_face.Update(in);

        // Formatted outside the lock: `strftime` is not expensive, and the rule
        // is that nothing avoidable happens while the display is held.
        char date[24] = {};
        if (view.time_valid) {
            const time_t now = static_cast<time_t>(in.epoch_utc);
            std::tm local = {};
            localtime_r(&now, &local);
            std::strftime(date, sizeof(date), "%a %d %b", &local);
        }

        // A minute with no document is the stream having stopped (§10.8.3), and
        // the screen leaves. Checked here rather than on arrival because it is a
        // thing that happens when nothing happens.
        if (g_limits.Tick(now_ms)) {
            g_nav.LimitsWentQuiet();
        }

        // **Gathered outside the lock, because one of these rows is an I²C
        // read** (§10.8.1), and only while the screen showing them is up: the
        // status page costs a bus transaction and the clock must not pay for it.
        // The page travels with the facts — `status_screen.h` says why.
        // **Whether a power-off would actually happen**, handed to the menu so
        // that the row can say `usb in` before anybody presses it rather than
        // after (§10.1). From the same snapshot the clock's battery icon uses,
        // so it costs no extra transaction.
        g_menu.SetCanPowerOff(battery_valid && !battery.vbus_present);

        // The touch test, and it is polled outside the LVGL lock for the reason
        // the status page is: it is an I²C read (§10.8.1).
        TouchView touch_view;
        if (g_nav.Screen() == ui::ScreenId::kTouch) {
            g_touch_flow.Tick(now_ms);
            PollTouch(&touch_view, now_ms);
        } else if (g_touch_down) {
            // A finger left on the glass while the screen changed under it is
            // not a press on whatever came next.
            g_touch_down = false;
        }

        StatusFacts facts;
        if (g_nav.Screen() == ui::ScreenId::kStatus) {
            facts.page = g_pager.Index();
            facts.page_count = ui::StatusPager::kPageCount;
            FillStatus(&facts, battery_valid, battery);
        }

        // **Nothing is painted while the panel is off.** The pixels would go to
        // a display nobody can see, over the QSPI they share with everything
        // else. Everything above still runs — the limits' quiet timer, the
        // card's countdown, the clock's own arithmetic — because those are facts
        // about the world rather than about the glass, and a device that came
        // back out of the blank into a stale screen would be worse than one that
        // never blanked.
        const bool dark = g_idle.State() == ui::DisplayPower::kOff;
        bool applied = false;
        if (!dark) {
            if (display::Lock lock(kLockTimeoutMs); lock) {
                // **What a finger did, taken under the same lock the callback that
                // recorded it ran under** — which is what makes the handoff need no
                // lock of its own. Acted on here rather than there: §10.8.1 keeps
                // every decision off the LVGL task, and one of these ends in
                // `esp_restart`.
                if (g_settings_screen.TakeBack() || g_status_screen.TakeBack()) {
                    Apply(ui::Nav::kBack);
                }
                const uint8_t tapped = g_settings_screen.TakeTap();
                if (tapped != SettingsScreen::kNoRow && g_nav.Screen() == ui::ScreenId::kSettings &&
                    !g_nav.RequestVisible()) {
                    // A tap is both the selection and the press, which is what
                    // `settings_menu.h` means when it says re-selecting the armed row
                    // must not disarm it.
                    g_menu.Select(tapped);
                    Activated(now_ms);
                }
                if (g_status_screen.TakeNext() && g_nav.Screen() == ui::ScreenId::kStatus) {
                    g_pager.Next();
                }
                if (g_gesture != LV_DIR_NONE) {
                    const lv_dir_t drawn = g_gesture;
                    g_gesture = LV_DIR_NONE;
                    Apply(NavForGesture(drawn));
                }

                g_clock.SetDate(date);
                g_clock.SetNotice(view.notice ? g_notice : "");
                g_clock.Apply(view);

                // Which of the two full-screen objects is up is the navigator's
                // answer, and applying it is the twenty lines `navigator.h` promised
                // somebody else would write.
                const ui::ScreenId up = g_nav.Screen();
                g_clock.SetVisible(up == ui::ScreenId::kClock);
                g_limits_screen.SetVisible(up == ui::ScreenId::kLimits);
                g_settings_screen.SetVisible(up == ui::ScreenId::kSettings);
                g_status_screen.SetVisible(up == ui::ScreenId::kStatus);
                g_touch_screen.SetVisible(up == ui::ScreenId::kTouch);
                g_settings_screen.Apply(g_menu, now_ms);
                if (up == ui::ScreenId::kStatus) {
                    g_status_screen.Apply(facts);
                }
                if (up == ui::ScreenId::kTouch) {
                    g_touch_screen.Apply(touch_view);
                }
                // The epoch, or 0 when it is not believable — which is what decides
                // whether a countdown is computed here or taken from what the
                // publisher resolved (§10.8.3).
                g_limits_screen.Apply(g_limits, view.time_valid ? in.epoch_utc : 0, now_ms);

                // The card last, so it is the last thing invalidated — and it sits on
                // top of a clock that has no idea it is there (§10.8.1).
                g_request_screen.Apply(g_card, now_ms, g_note);
                applied = true;
            }
        }

        // A pass skipped because the panel is off is not a frame given up
        // waiting for the display, and the counter that says so is the one
        // §10.12.2 reads.
        Publish(view, applied || dark);

        ++tick;
        vTaskDelay(pdMS_TO_TICKS(kTickMs));
    }
}

}  // namespace

esp_err_t Init(const Hardware &hardware, const Keys &keys) {
    if (g_handle != nullptr) {
        return ESP_OK;
    }
    if (!display::LvglReady()) {
        // Not a failure of this component: a board whose panel did not come up
        // still boots, still answers the console and still speaks to the bus
        // (§10.10's rule about staying up to report).
        ESP_LOGW(TAG, "no display, no screens");
        return ESP_ERR_INVALID_STATE;
    }

    g_hardware = hardware;
    g_battery = hardware.battery;
    g_keys = keys;
    g_alert = hardware.alert;
    g_touch = hardware.touch;
    g_panel = hardware.panel;

    // The idle timer starts now rather than at zero: `Init` runs several seconds
    // into the boot, after the splash and the chime, and a policy that thought
    // it had been idle since the epoch would dim the first screen the operator
    // ever sees.
    ApplyDisplaySettings();
    g_idle.Activity(static_cast<uint32_t>(esp_timer_get_time() / 1000));

    if (g_lock == nullptr) {
        g_lock = xSemaphoreCreateMutexStatic(&g_lock_storage);
        if (g_lock == nullptr) {
            return ESP_ERR_NO_MEM;
        }
    }

    {
        display::Lock lock(kLockTimeoutMs);
        if (!lock) {
            ESP_LOGE(TAG, "LVGL busy, screens not built");
            return ESP_ERR_TIMEOUT;
        }
        lv_obj_t *screen = lv_screen_active();
        // Black, and it is the whole point rather than a taste: on an AMOLED an
        // unlit pixel costs no power and no lifetime (§10.8.1).
        lv_obj_set_style_bg_color(screen, lv_color_black(), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);

        const esp_err_t err = g_clock.Create(screen);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "clock not built: %s", esp_err_to_name(err));
            return err;
        }

        // Between the clock and the card in sibling order, which is the order they
        // outrank each other in: the limits cover the clock, the card covers both.
        const esp_err_t limits = g_limits_screen.Create(screen);
        if (limits != ESP_OK) {
            ESP_LOGE(TAG, "limits not built: %s", esp_err_to_name(limits));
            return limits;
        }

        // The two of §10.8.5, still under the card and over nothing: exactly one
        // full-screen object is visible and the navigator is what picks it.
        const esp_err_t settings = g_settings_screen.Create(screen);
        if (settings != ESP_OK) {
            ESP_LOGE(TAG, "settings not built: %s", esp_err_to_name(settings));
            return settings;
        }
        const esp_err_t status = g_status_screen.Create(screen);
        if (status != ESP_OK) {
            ESP_LOGE(TAG, "status not built: %s", esp_err_to_name(status));
            return status;
        }
        const esp_err_t touch = g_touch_screen.Create(screen);
        if (touch != ESP_OK) {
            ESP_LOGE(TAG, "touch test not built: %s", esp_err_to_name(touch));
            return touch;
        }

        // After the clock, so it is the later sibling and therefore the one LVGL
        // draws on top (§10.8.1: the card outranks everything).
        const esp_err_t card = g_request_screen.Create(screen);
        if (card != ESP_OK) {
            ESP_LOGE(TAG, "request card not built: %s", esp_err_to_name(card));
            return card;
        }

        // **The gesture lands on the screen object, not on ours.** LVGL sends
        // `LV_EVENT_GESTURE` to whatever clickable object the press started on;
        // our full-screen roots are deliberately not clickable, so a swipe over
        // any of them finds the screen underneath — which is the one place a
        // handler belongs, because a swipe is about *which* screen rather than
        // about the one it started on. The rows of the settings list are
        // clickable and therefore swallow gestures, which is exactly right:
        // §10.8.1 forbids swipe navigation on that screen anyway.
        lv_obj_add_event_cb(screen, ScreenGesture, LV_EVENT_GESTURE, nullptr);
    }

    // The chirp's task, before the screen task: a card cannot go up until the
    // latter is running, so there is no window in which one arrives and finds
    // nowhere to ring. A device with no codec still gets both — `AlertTask` looks
    // at the speaker each time rather than at boot, so a codec that came up late
    // or not at all is one silent card rather than a branch here.
    if (g_alert_signal == nullptr) {
        g_alert_signal = xSemaphoreCreateBinaryStatic(&g_alert_signal_storage);
        if (g_alert_signal == nullptr) {
            return ESP_ERR_NO_MEM;
        }
        // Below the screen task: a noise is never more urgent than the press it
        // is announcing.
        xTaskCreateStatic(AlertTask, "card-alert", sizeof(g_alert_stack) / sizeof(g_alert_stack[0]),
                          nullptr, kTaskPriority - 1, g_alert_stack, &g_alert_tcb);
    }

    g_handle = xTaskCreateStatic(Task, "screens", sizeof(g_stack) / sizeof(g_stack[0]), nullptr,
                                 kTaskPriority, g_stack, &g_task);
    if (g_handle == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "clock up, %" PRId32 "x%" PRId32 " drifting +-%d/+-%d", kFaceWidth, kFaceHeight,
             static_cast<int>(ui::ClockFace::kDriftX), static_cast<int>(ui::ClockFace::kDriftY));
    return ESP_OK;
}

bool Ready() { return g_handle != nullptr; }

void ApplyDisplaySettings() {
    const config::Display &display = config::Get().display;

    ui::IdleSettings settings;
    settings.dim_after_ms = static_cast<uint32_t>(display.dim_after_seconds) * 1000U;
    settings.sleep_after_ms = static_cast<uint32_t>(display.sleep_after_seconds) * 1000U;
    settings.full_percent = display.brightness;
    settings.dim_percent = display.dim_percent;
    g_idle.Configure(settings);

    // **The level can change without the state changing**, which is the whole
    // reason this flag exists: `config set brightness 50` on a lit screen leaves
    // the policy in `kFull` and means a different number, and a device that only
    // acted on transitions would show it after the next idle timeout instead of
    // now.
    g_display_dirty = true;
}

bool Inject(const ui::Request &request) {
    if (g_handle == nullptr) {
        return false;
    }
    const uint32_t now_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000);

    // **A request wakes the panel unconditionally** (§10.8.1), and before the
    // card is even queued: a device that lit up a moment after the chirp would
    // be a device whose first frame of the one screen it exists for is missing.
    g_idle.Activity(now_ms);

    // **Under the snapshot lock and not the display lock.** The queue is read by
    // the task on every tick and the screen catches up on its own next pass, at
    // most 20 ms away — which is what keeps this call from waiting behind a frame.
    if (xSemaphoreTake(g_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        return false;
    }
    const bool accepted = g_card.Arrived(request, now_ms);
    if (accepted) {
        g_nav.RequestArrived();
    }
    xSemaphoreGive(g_lock);

    // **Only a card that is actually going up makes a noise** (§10.8.1: never
    // chirp for one that was already there). A refusal is silent, which is also
    // the honest thing: nothing appeared.
    if (accepted && g_alert_signal != nullptr) {
        xSemaphoreGive(g_alert_signal);
    }

    if (!accepted) {
        // §10.10: one log line, no reply. Which of the refusals it was shows up
        // in the counters, and `request` on the console prints them.
        ESP_LOGW(TAG, "request refused: full, oversized, or nothing to answer into");
    }
    return accepted;
}

CardStatus Card() {
    CardStatus out;
    if (g_handle == nullptr || g_lock == nullptr) {
        return out;
    }
    if (xSemaphoreTake(g_lock, pdMS_TO_TICKS(50)) != pdTRUE) {
        return out;
    }

    const uint32_t now_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000);
    out.ready = true;
    out.state = g_card.State();
    out.pending = g_card.Pending();
    out.waiting = g_card.Waiting();
    out.remaining_ms = g_card.RemainingMs(now_ms);
    if (const ui::Request *front = g_card.Front(); front != nullptr) {
        std::snprintf(out.tool, sizeof(out.tool), "%s", front->tool_name);
        std::snprintf(out.cwd, sizeof(out.cwd), "%s", front->cwd);
        // **Copied rather than `snprintf`ed, because the truncation is the
        // point** and `-Wformat-truncation` is right to object to a format that
        // silently loses 2 KB. Cutting it here explicitly, next to the length that
        // says how much was cut, is the honest spelling of it.
        const size_t length = std::strlen(front->tool_input);
        const size_t room = sizeof(out.input_preview) - 1;
        const size_t take = length < room ? length : room;
        std::memcpy(out.input_preview, front->tool_input, take);
        out.input_preview[take] = '\0';
        out.input_length = static_cast<uint16_t>(length);
    }
    out.last_outcome = g_card.LastOutcome();
    std::snprintf(out.last_tool, sizeof(out.last_tool), "%s", g_card.LastTool());
    out.allowed = g_card.Allowed();
    out.denied = g_card.Denied();
    out.timed_out = g_card.TimedOut();
    out.refused = g_card.Refused();
    out.ignored = g_card.Ignored();

    xSemaphoreGive(g_lock);
    return out;
}

Status Get() {
    Status copy;
    if (g_lock == nullptr) {
        return copy;
    }
    if (xSemaphoreTake(g_lock, pdMS_TO_TICKS(50)) != pdTRUE) {
        // A readout that waits is a readout nobody runs twice. An empty answer
        // is honest: `ready` false says the task has not filled it in.
        return copy;
    }
    copy = g_status;
    xSemaphoreGive(g_lock);
    return copy;
}

void OnDecision(DecisionHandler handler, void *user) {
    // No lock: this is called once from `main` before anything can press a
    // button, and a hook that could be swapped while a decision was in flight
    // would be a design with a race in it rather than a missing critical section.
    g_handler = handler;
    g_handler_user = user;
}

void SetReceiptNote(const char *note) {
    if (note == nullptr) {
        return;
    }
    // Copied rather than pointed at, and truncated rather than refused — this is
    // the one string on this screen where a short version still says the true
    // thing, and the alternative to truncating is a receipt with nothing on it.
    std::snprintf(g_note, sizeof g_note, "%s", note);
}

MenuStatus Menu() {
    MenuStatus out;
    if (g_handle == nullptr || g_lock == nullptr) {
        return out;
    }
    if (xSemaphoreTake(g_lock, pdMS_TO_TICKS(50)) != pdTRUE) {
        return out;
    }
    out.ready = true;
    out.screen = g_nav.Screen();
    out.selected = g_menu.Selected();
    out.armed = g_menu.Armed(static_cast<uint32_t>(esp_timer_get_time() / 1000));
    out.can_power_off = g_menu.CanPowerOff();
    out.status_page = g_pager.Index();
    out.status_pages = ui::StatusPager::kPageCount;
    xSemaphoreGive(g_lock);
    return out;
}

bool Navigate(ui::Nav nav) {
    if (g_handle == nullptr) {
        return false;
    }
    // Queued rather than applied, so that the navigator is only ever moved by the
    // one task that owns it — the same reason a tap is recorded and not acted on.
    g_pending_nav = nav;
    g_has_pending_nav = true;
    // Somebody asked for a screen, so it had better be visible — the one place
    // the console counts as activity, because unlike `config set` it is a
    // request to look at something rather than a setting typed at it.
    Activity();

    // **And waited for, because there is one slot and callers chain.** Reaching
    // the status pages is "up, then open" — two moves — and a second call that
    // overwrote the first before the task had seen it would silently perform
    // only the last one. Found on the board: `screen status` from the limits
    // screen did nothing at all, twice, and the readout printed afterwards was
    // the honest answer to a question nobody had asked.
    //
    // Bounded, and a timeout is not an error worth a branch: the caller reads
    // the screen back afterwards, which is a better answer than this one.
    for (int waited = 0; g_has_pending_nav && waited < 20; ++waited) {
        vTaskDelay(pdMS_TO_TICKS(kTickMs));
    }
    return !g_has_pending_nav;
}

void ResetTouch() { ResetTouchCalibration(); }

bool NextStatusPage() {
    if (g_handle == nullptr || g_nav.Screen() != ui::ScreenId::kStatus) {
        return false;
    }
    g_pager.Next();
    return true;
}

void SetNotice(const char *text) {
    // The clock is what carries it, so a device with no panel has nothing to
    // do here — and `main` calls this after `Init`, so there always is one when
    // it matters.
    if (g_handle == nullptr) {
        return;
    }
    // A line worth putting under the clock is a line worth being able to read.
    Activity();
    if (text == nullptr || text[0] == '\0') {
        g_notice_set = false;
        g_notice[0] = '\0';
        return;
    }
    // Truncated rather than refused, the same call `SetReceiptNote` makes and
    // for the same reason: a shortened headline still says the true thing, and
    // the alternative is a boot that says nothing happened.
    std::snprintf(g_notice, sizeof g_notice, "%s", text);
    g_notice_since_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000);
    g_notice_set = true;
}

void ShowLimits(const ui::Limits &limits) {
    if (g_handle == nullptr) {
        return;
    }
    const uint32_t now_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000);

    // §9.7 publishes on every render, so a document arriving means somebody is
    // working — which is the operator's own definition of the device not being
    // idle, and it holds whether or not the screen comes up.
    g_idle.Activity(now_ms);

    // Under the snapshot lock and not the display lock, exactly as `Inject` is:
    // the task picks it up on its next pass, at most 20 ms away, and this call
    // never waits behind a frame.
    if (xSemaphoreTake(g_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        return;
    }
    const bool raise = g_limits.Arrived(limits, now_ms);
    if (raise) {
        // The navigator has the last word: a request card outranks a readout, and
        // an operator in settings is not thrown out of them (§10.8.1).
        g_nav.LimitsArrived();
    }
    xSemaphoreGive(g_lock);
}

LimitsStatus Limits() {
    LimitsStatus out;
    if (g_handle == nullptr) {
        return out;
    }
    const uint32_t now_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000);
    if (xSemaphoreTake(g_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        return out;
    }
    out.ready = true;
    out.on_screen = g_nav.Screen() == ui::ScreenId::kLimits;
    out.has_document = g_limits.HasDocument();
    out.quiet = g_limits.Quiet();
    out.dismissed = g_limits.DismissedNow();
    out.received = g_limits.Received();
    out.age_ms = g_limits.AgeMs(now_ms);
    out.document = g_limits.Document();
    xSemaphoreGive(g_lock);
    return out;
}

}  // namespace screens
