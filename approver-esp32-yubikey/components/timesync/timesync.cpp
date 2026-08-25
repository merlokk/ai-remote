#include "timesync.h"

#include <cstring>
#include <sys/time.h>

#include "config.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sync_policy.h"
#include "wifi_manager.h"

namespace timesync {

namespace {

constexpr const char *TAG = "timesync";

// 4 KB in bytes, like the manager's: this task logs, calls into lwIP's SNTP
// client and writes seven registers over I²C. Nothing recursive, nothing deep.
constexpr uint32_t kStackBytes = 4096;

StackType_t task_stack[kStackBytes];
StaticTask_t task_storage;
TaskHandle_t task_handle = nullptr;

StaticSemaphore_t lock_storage;
SemaphoreHandle_t lock = nullptr;

SyncPolicy policy;
bool started = false;

// Slower than the Wi-Fi manager's 200 ms, and deliberately: the fastest thing
// this has to notice is a link coming up, and a second late on a clock that is
// about to be corrected by seconds is not a number anybody can perceive.
constexpr uint32_t kPollMs = 1000;

// **Our own bound on somebody else's wait** (§10.5). `esp_netif_sntp_sync_wait`
// takes a timeout, so this is what that timeout is: long enough for a DNS
// lookup and a retry on a slow link, short enough that a device with no route
// is not sitting in this call for a minute at a time.
constexpr uint32_t kExchangeMs = 15000;

// What this RTC can hold (`pcf85063.h`: two digits and no century), used here
// as a sanity check on what came back. A server answering 1970 or 2107 is a
// server to disbelieve — §10.8.2's rule that an obviously unset clock beats a
// plausible wrong one, applied to an answer rather than to a chip.
constexpr int kEarliestYear = 2024;
constexpr int kLatestYear = 2099;

// What the last exchange produced. Written only from the task, and only under
// the lock — a `time_t` is 64-bit on a 32-bit core, so a console reading one
// while the task writes it is a torn number rather than a race that cannot
// happen.
esp_err_t last_error = ESP_OK;
time_t last_utc = 0;
int32_t last_step_seconds = 0;

// One exchange's result, carried out of `RunExchange` so that the commit
// happens in one place with the lock held.
struct Outcome {
    bool ok = false;
    esp_err_t error = ESP_OK;
    time_t utc = 0;
    int32_t step_seconds = 0;
};

uint32_t NowMs() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }

SyncSettings SettingsFromConfig() {
    const config::Time &time_config = config::Get().time;
    SyncSettings settings;
    // **Two ways to have nothing to do, and both are the file saying so**: an
    // interval of zero is "do not sync", and an empty server is "there is
    // nothing to ask". Neither is a second switch that can disagree with the
    // first — they are the two halves of the same setting being absent.
    settings.enabled = time_config.sync_hours > 0 && time_config.sntp_server[0] != '\0';
    settings.interval_ms = static_cast<uint32_t>(time_config.sync_hours) * 3600u * 1000u;
    return settings;
}

// Is there something to ask through? **Not the same question as "is the link
// up"**, and not quite "is the internet check happy" either:
//
//   * no client link with an address — nothing to send through, so no;
//   * `kOffline` — something already asked and got nothing back, so no;
//   * `kUnknown` — the check is switched off, or has not answered yet. **Yes**,
//     and that is the interesting case: a device whose operator turned the ping
//     check off would otherwise have quietly lost its clock as well, and an
//     SNTP exchange is its own reachability test.
bool InternetIsUsable() {
    if (!wifimgr::Ready()) {
        return false;
    }
    const wifimgr::Snapshot snapshot = wifimgr::Get();
    const bool link_up = snapshot.radio.mode == wifi::Mode::kClient &&
                         snapshot.radio.link == wifi::Link::kConnected && snapshot.radio.ip != 0;
    return link_up && snapshot.internet != wifimgr::Internet::kOffline;
}

// One exchange, start to finish. Blocking, in a task of its own, and never
// with a lock held — the console has to stay answerable while this runs.
Outcome RunExchange() {
    Outcome outcome;

    char server[config::kHostSize] = {};
    xSemaphoreTake(lock, portMAX_DELAY);
    snprintf(server, sizeof(server), "%s", config::Get().time.sntp_server);
    xSemaphoreGive(lock);

    const char *name = server;
    esp_sntp_config_t sntp_config = ESP_NETIF_SNTP_DEFAULT_CONFIG(name);
    esp_err_t err = esp_netif_sntp_init(&sntp_config);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SNTP would not start: %s", esp_err_to_name(err));
        outcome.error = err;
        return outcome;
    }

    // **Two clocks, and the second one is the point.** `time()` is what the
    // exchange is about to move; `esp_timer_get_time()` is monotonic since
    // boot and no `settimeofday` touches it. Taking both is what lets the
    // correction be separated from the seconds the exchange itself spent —
    // see where the step is computed below.
    const time_t before = time(nullptr);
    const int64_t started_us = esp_timer_get_time();

    err = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(kExchangeMs));
    // **Deinitialised on every path.** The client keeps a socket and a
    // semaphore, and a second `init` over a live one is refused — so leaking it
    // once would mean never syncing again.
    esp_netif_sntp_deinit();

    if (err != ESP_OK) {
        // A hostname that will not resolve looks exactly like a server that
        // did not answer, and from here they are the same outcome: no time.
        ESP_LOGW(TAG, "no answer from %s within %u ms", server,
                 static_cast<unsigned>(kExchangeMs));
        outcome.error = err;
        return outcome;
    }

    const time_t after = time(nullptr);
    struct tm fields = {};
    gmtime_r(&after, &fields);
    if (fields.tm_year + 1900 < kEarliestYear || fields.tm_year + 1900 > kLatestYear) {
        // Refused rather than stored, and counted as a failure: §10.8.2's rule
        // that a plausible wrong time is worse than an obviously unset one
        // applies to an answer as much as to a chip.
        ESP_LOGW(TAG, "%s answered %04d, which this clock will not hold; ignored", server,
                 fields.tm_year + 1900);
        outcome.error = ESP_ERR_INVALID_RESPONSE;
        return outcome;
    }

    outcome.ok = true;
    outcome.utc = after;

    // **How far the server moved the clock — not how long the exchange took.**
    // The naive `after - before` is both added together, because `time()` runs
    // normally during the seconds spent resolving a name and waiting on a
    // packet. On this board that reported **+5 s** twenty-six seconds after a
    // sync that had already corrected the clock, which is a drift rate no
    // crystal has; the five seconds were the exchange. Subtracting the
    // monotonic elapsed time leaves the correction alone, which is the number
    // §10.8.2 wants: it is how an RTC to be suspicious of announces itself.
    // Rounded, not truncated: a 4.6-second exchange counted as 4 leaves 0.6 s
    // of itself in the answer, which is how a device with a perfect clock came
    // to report `+1 s`. The granularity is still a second — this is a
    // diagnostic, not a measurement of the crystal.
    const int64_t elapsed_s = (esp_timer_get_time() - started_us + 500000) / 1000000;
    outcome.step_seconds = static_cast<int32_t>(after - before - elapsed_s);


    ESP_LOGI(TAG, "clock set from %s: %04d-%02d-%02d %02d:%02d:%02d UTC (%+d s)", server,
             fields.tm_year + 1900, fields.tm_mon + 1, fields.tm_mday, fields.tm_hour,
             fields.tm_min, fields.tm_sec, static_cast<int>(outcome.step_seconds));
    return outcome;
}

void Task(void *) {
    for (;;) {
        const bool usable = InternetIsUsable();

        xSemaphoreTake(lock, portMAX_DELAY);
        policy.OnInternet(usable, NowMs());
        const Action action = policy.Tick(NowMs());
        xSemaphoreGive(lock);

        if (action == Action::kSync) {
            // **Outside the lock**, because it blocks for up to fifteen
            // seconds. `Tick` has already marked the exchange as outstanding,
            // so nothing else can start a second one while this runs.
            const Outcome outcome = RunExchange();
            xSemaphoreTake(lock, portMAX_DELAY);
            last_error = outcome.error;
            if (outcome.ok) {
                last_utc = outcome.utc;
                last_step_seconds = outcome.step_seconds;
            }
            policy.OnResult(outcome.ok, NowMs());
            xSemaphoreGive(lock);
        }

        vTaskDelay(pdMS_TO_TICKS(kPollMs));
    }
}

}  // namespace

esp_err_t Init() {
    if (started) {
        return ESP_OK;
    }

    lock = xSemaphoreCreateMutexStatic(&lock_storage);
    if (lock == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    policy.Configure(SettingsFromConfig(), NowMs());

    task_handle =
        xTaskCreateStatic(&Task, "timesync", kStackBytes, nullptr, 3, task_stack, &task_storage);
    if (task_handle == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    started = true;

    const config::Time &time_config = config::Get().time;
    if (time_config.sync_hours == 0) {
        ESP_LOGI(TAG, "clock sync is off; this board has no RTC, so the time will not be set at all");
    } else if (time_config.sntp_server[0] == '\0') {
        // Not an error and not retried: no server named is the operator saying
        // there is nothing to ask, and a device that tried anyway would log a
        // failure every interval for the life of the board.
        ESP_LOGI(TAG, "no time server set; the clock will not sync");
    } else {
        ESP_LOGI(TAG, "clock sync every %u h from %s, and whenever the internet comes back",
                 static_cast<unsigned>(time_config.sync_hours), time_config.sntp_server);
    }
    return ESP_OK;
}

bool Ready() { return started; }

void Apply() {
    if (!started) {
        return;
    }
    xSemaphoreTake(lock, portMAX_DELAY);
    policy.Configure(SettingsFromConfig(), NowMs());
    xSemaphoreGive(lock);
}

void SyncNow() {
    if (!started) {
        return;
    }
    xSemaphoreTake(lock, portMAX_DELAY);
    policy.SyncNow(NowMs());
    xSemaphoreGive(lock);
}

Status Get() {
    Status status;
    if (!started) {
        return status;
    }

    const uint32_t now = NowMs();
    xSemaphoreTake(lock, portMAX_DELAY);
    status.enabled = policy.Enabled();
    status.internet = policy.InternetIsUsable();
    status.syncing = policy.Syncing();
    status.ever_synced = policy.EverSynced();
    status.since_last_ms = policy.SinceLastSyncMs(now);
    status.next_in_ms = policy.NextSyncInMs(now);
    status.successes = policy.Successes();
    status.failures = policy.FailuresInARow();
    status.last_utc = last_utc;
    status.last_step_seconds = last_step_seconds;
    status.last_error = last_error;
    xSemaphoreGive(lock);
    return status;
}

}  // namespace timesync
