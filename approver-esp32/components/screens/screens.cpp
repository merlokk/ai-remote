#include "screens.h"

#include <cinttypes>
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
pmic::Axp2101 *g_battery = nullptr;

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

void Task(void *) {
    ui::ClockInputs in;
    pmic::Status battery = {};
    bool battery_valid = false;
    uint32_t tick = 0;

    for (;;) {
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
            applied = true;
        }

        if (xSemaphoreTake(g_lock, pdMS_TO_TICKS(20)) == pdTRUE) {
            g_status.ready = true;
            g_status.view = view;
            g_status.updates++;
            if (!applied) {
                g_status.lock_misses++;
            }
            g_status.stack_low_water = uxTaskGetStackHighWaterMark(nullptr);
            xSemaphoreGive(g_lock);
        }

        ++tick;
        vTaskDelay(pdMS_TO_TICKS(kTickMs));
    }
}

}  // namespace

esp_err_t Init(pmic::Axp2101 *battery) {
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
