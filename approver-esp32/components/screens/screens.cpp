#include "screens.h"

#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "clock_face.h"
#include "clock_screen.h"
#include "limits_screen.h"
#include "esp_log.h"
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

// **The navigator is kept in step with the card rather than owning the queue.**
// §10.8.1 gives it the rule that navigation vanishes while a card is up, and it
// deliberately does not hold the requests themselves ("a navigator that stored
// `tool_name` would be a navigator with the protocol in it"). So the card queue
// is where they live and this is told when one arrives, is answered or expires —
// the count in each is the same count, stepped in one place.
ui::Navigator g_nav;

pmic::Axp2101 *g_battery = nullptr;
Keys g_keys;

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
char g_note[kNoteSize] = "decided, not sent - nothing is listening";

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

    if (g_keys.buttons->Poll(g_keys.allow) == buttons::Event::kPressed) {
        if (g_card.Press(ui::Verdict::kAllow, now_ms, &g_answered)) {
            g_nav.RequestAnswered();
            Decided(g_answered, ui::Verdict::kAllow);
        }
    }

    if (g_keys.buttons->Poll(g_keys.deny) == buttons::Event::kPressed) {
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
            // Otherwise it is the way home — and from the limits screen that is
            // also a **dismissal**: §10.8.3's numbers arrive every few seconds
            // while a session is working, so a back that only changed the screen
            // would be undone before the finger left the button. `Dismissed`
            // makes it last until the stream goes quiet, which is the only
            // reading in which the button does anything (`limits_view.h`).
            const bool leaving_limits = g_nav.Screen() == ui::ScreenId::kLimits;
            if (g_nav.Navigate(ui::Nav::kBack) && leaving_limits) {
                g_limits.Dismissed();
            }
        }
    }
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

        // Most ticks are a button poll and nothing else. A card that just changed
        // is drawn immediately rather than at the next face pass: an arrival, a
        // press and an expiry are all things the operator is looking at.
        if ((tick % kFaceEveryTicks) != 0 && !card_changed) {
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

        bool applied = false;
        if (display::Lock lock(kLockTimeoutMs); lock) {
            g_clock.SetDate(date);
            g_clock.Apply(view);

            // Which of the two full-screen objects is up is the navigator's
            // answer, and applying it is the twenty lines `navigator.h` promised
            // somebody else would write.
            const bool limits_up = g_nav.Screen() == ui::ScreenId::kLimits;
            g_clock.SetVisible(!limits_up);
            g_limits_screen.SetVisible(limits_up);
            // The epoch, or 0 when it is not believable — which is what decides
            // whether a countdown is computed here or taken from what the
            // publisher resolved (§10.8.3).
            g_limits_screen.Apply(g_limits, view.time_valid ? in.epoch_utc : 0, now_ms);

            // The card last, so it is the last thing invalidated — and it sits on
            // top of a clock that has no idea it is there (§10.8.1).
            g_request_screen.Apply(g_card, now_ms, g_note);
            applied = true;
        }

        Publish(view, applied);

        ++tick;
        vTaskDelay(pdMS_TO_TICKS(kTickMs));
    }
}

}  // namespace

esp_err_t Init(pmic::Axp2101 *battery, const Keys &keys, audio::Speaker *alert) {
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

    g_battery = battery;
    g_keys = keys;
    g_alert = alert;

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

        // After the clock, so it is the later sibling and therefore the one LVGL
        // draws on top (§10.8.1: the card outranks everything).
        const esp_err_t card = g_request_screen.Create(screen);
        if (card != ESP_OK) {
            ESP_LOGE(TAG, "request card not built: %s", esp_err_to_name(card));
            return card;
        }
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

bool Inject(const ui::Request &request) {
    if (g_handle == nullptr) {
        return false;
    }
    const uint32_t now_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000);

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

void ShowLimits(const ui::Limits &limits) {
    if (g_handle == nullptr) {
        return;
    }
    const uint32_t now_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000);

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
