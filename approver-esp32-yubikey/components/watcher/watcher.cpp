#include "watcher.h"

#include "activity.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nats_link.h"
#include "status.h"

namespace watcher {
namespace {

constexpr const char *TAG = "watcher";

StaticSemaphore_t g_lock_storage;
SemaphoreHandle_t g_lock = nullptr;

Status g_status;
bool g_started = false;

// **Two subjects, two flags, one connection.** They are subscribed in the same
// pass and dropped together by a reconnect, and they are still counted apart: a
// device that is watching one and not the other is a state worth being able to see
// on the console rather than one to average away.
bool g_subscribed = false;
bool g_subscribed_activity = false;
uint16_t g_subscribed_at = 0;

// Parsed on the bus task, so these are statics rather than a kilobyte of somebody
// else's stack (§10.14.1) — the same call `responder.cpp` makes about the request
// it parses there.
ui::Limits g_incoming;
ui::Activity g_incoming_activity;

// The last one of each that parsed. **Held here rather than pushed anywhere**:
// this board has no screen to push to, so the console pulls (see `watcher.h`).
ui::Limits g_limits;
ui::Activity g_activity;
bool g_has_limits = false;
bool g_has_activity = false;
uint32_t g_limits_at_ms = 0;
uint32_t g_activity_at_ms = 0;

void Lock() { xSemaphoreTake(g_lock, portMAX_DELAY); }
void Unlock() { xSemaphoreGive(g_lock); }

uint32_t NowMs() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }

void Adopt(const ui::Limits &limits) {
    Lock();
    g_limits = limits;
    g_has_limits = true;
    g_limits_at_ms = NowMs();
    Unlock();
}

void AdoptActivity(const ui::Activity &activity) {
    Lock();
    g_activity = activity;
    g_has_activity = true;
    g_activity_at_ms = NowMs();
    Unlock();
}

void OnStatus(const nats::Message &message, void *) {
    Lock();
    ++g_status.received;
    Unlock();

    if (!protocol::ParseStatus(message.data, message.size, &g_incoming)) {
        // **No log line per message here**, unlike the approval path, and the
        // difference is the traffic: `status` is published on every render of a
        // status line, so a publisher this device cannot read would fill the log
        // several times a second. The counter is the readout, and `limits` prints
        // it.
        Lock();
        ++g_status.refused;
        Unlock();
        return;
    }

    Adopt(g_incoming);
}

// §9.10's other document — what the session is *doing*. Everything above holds
// here: parsed on this task, counted rather than logged, and kept as the last good
// one.
//
// **It raises nothing, and on this board neither does the other one.** On the C6
// firmware `ShowLimits` could bring a screen up and this could only update a line
// on it; here both are the same kind of thing, which is a struct in memory that
// `limits` on the console prints.
void OnActivity(const nats::Message &message, void *) {
    Lock();
    ++g_status.activity_received;
    Unlock();

    if (!protocol::ParseActivity(message.data, message.size, &g_incoming_activity)) {
        Lock();
        ++g_status.activity_refused;
        Unlock();
        return;
    }

    AdoptActivity(g_incoming_activity);
}

}  // namespace

const char *BlockerText(Blocker blocker) {
    switch (blocker) {
        case Blocker::kNone:
            return "nothing";
        case Blocker::kNoBus:
            return "not connected to the bus - see 'nats'";
    }
    return "unknown";
}

esp_err_t Init() {
    if (g_started) {
        return ESP_OK;
    }
    g_lock = xSemaphoreCreateMutexStatic(&g_lock_storage);
    if (g_lock == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    g_started = true;
    Lock();
    g_status.ready = true;
    Unlock();
    return ESP_OK;
}

bool Ready() { return g_started; }

void Maintain() {
    if (!g_started) {
        return;
    }
    const nats::Status bus = nats::Get();
    const bool connected = bus.state == nats::State::kConnected;

    // A reconnect drops every subscription with the client, so "subscribed" is
    // only true for the connection it was made on — the same bookkeeping
    // `responder.cpp` does, and for the same reason.
    const bool lost = !connected || bus.connects != g_subscribed_at;
    if (lost && (g_subscribed || g_subscribed_activity)) {
        if (connected) {
            if (g_subscribed) {
                nats::Unsubscribe(protocol::kStatusSubject);
            }
            if (g_subscribed_activity) {
                nats::Unsubscribe(protocol::kActivitySubject);
            }
        }
        g_subscribed = false;
        g_subscribed_activity = false;
        ESP_LOGI(TAG, "no longer watching %s or %s", protocol::kStatusSubject,
                 protocol::kActivitySubject);
    }

    if (connected) {
        // **No queue group** (§10.5, §10.8.3): a broadcast current value is meant
        // to reach every subscriber, and joining a group would mean taking it from
        // `approver-web` and whoever else is watching.
        if (!g_subscribed &&
            nats::Subscribe(protocol::kStatusSubject, nullptr, &OnStatus, nullptr) == ESP_OK) {
            g_subscribed = true;
            g_subscribed_at = bus.connects;
            ESP_LOGI(TAG, "watching %s", protocol::kStatusSubject);
        }
        // Each is tried on its own, so a subscribe that is refused once — the
        // client's table full, a socket that went away between the two calls —
        // costs the other one nothing and is retried on the next tick.
        if (!g_subscribed_activity &&
            nats::Subscribe(protocol::kActivitySubject, nullptr, &OnActivity, nullptr) == ESP_OK) {
            g_subscribed_activity = true;
            g_subscribed_at = bus.connects;
            ESP_LOGI(TAG, "watching %s", protocol::kActivitySubject);
        }
    }

    Lock();
    g_status.subscribed = g_subscribed;
    g_status.activity_subscribed = g_subscribed_activity;
    g_status.blocked_by = connected ? Blocker::kNone : Blocker::kNoBus;
    Unlock();
}

ui::Limits Limits(bool *has_limits) {
    ui::Limits copy;
    Lock();
    copy = g_limits;
    if (has_limits != nullptr) {
        *has_limits = g_has_limits;
    }
    Unlock();
    return copy;
}

ui::Activity Activity(bool *has_activity) {
    ui::Activity copy;
    Lock();
    copy = g_activity;
    if (has_activity != nullptr) {
        *has_activity = g_has_activity;
    }
    Unlock();
    return copy;
}

Status Get() {
    if (!g_started) {
        return Status{};
    }
    Lock();
    Status copy = g_status;
    // Filled here rather than kept in `g_status`, because an age is a thing that
    // changes with no event behind it — storing one would mean a field that is
    // stale by however long it has been since the last publish.
    copy.limits_age_ms = g_has_limits ? NowMs() - g_limits_at_ms : kNeverArrived;
    copy.activity_age_ms = g_has_activity ? NowMs() - g_activity_at_ms : kNeverArrived;
    Unlock();
    return copy;
}

}  // namespace watcher
