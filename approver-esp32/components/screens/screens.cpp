#include "screens.h"

#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "clock_face.h"
#include "clock_screen.h"
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

// Until §10.6 exists there is no key on this device, so a press decides and
// nothing leaves. The receipt says exactly that rather than "sent", because that
// is the one lie on this screen that would matter (§10.10).
constexpr const char *kNoSigner = "decided, not sent - no key yet";

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

// One decision, and today it goes into the log because there is nowhere else for
// it to go. **This is the seam §10.6 and §7 take over**: sign the request with
// the eFuse-derived key, publish into `request.reply`, flush. Nothing about the
// screen changes when that happens, which is the point of it being one function.
void Decided(const ui::Request &request, ui::Verdict verdict) {
    ESP_LOGW(TAG, "%s %s in %s - nothing published: this device has no key yet",
             verdict == ui::Verdict::kAllow ? "ALLOW" : "DENY", request.tool_name, request.cwd);
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
            // Otherwise it is the way home. Today the clock is the only screen
            // there is, so this is a no-op that is already right — wired now
            // rather than when the other four arrive, because the rule is about
            // the button and not about the screens.
            g_nav.Navigate(ui::Nav::kBack);
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

        bool applied = false;
        if (display::Lock lock(kLockTimeoutMs); lock) {
            g_clock.SetDate(date);
            g_clock.Apply(view);
            // The card last, so it is the last thing invalidated — and it sits on
            // top of a clock that has no idea it is there (§10.8.1).
            g_request_screen.Apply(g_card, now_ms, kNoSigner);
            applied = true;
        }

        Publish(view, applied);

        ++tick;
        vTaskDelay(pdMS_TO_TICKS(kTickMs));
    }
}

}  // namespace

esp_err_t Init(pmic::Axp2101 *battery, const Keys &keys) {
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

        // After the clock, so it is the later sibling and therefore the one LVGL
        // draws on top (§10.8.1: the card outranks everything).
        const esp_err_t card = g_request_screen.Create(screen);
        if (card != ESP_OK) {
            ESP_LOGE(TAG, "request card not built: %s", esp_err_to_name(card));
            return card;
        }
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

}  // namespace screens
