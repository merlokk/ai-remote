#include "watcher.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nats_link.h"
#include "screens.h"
#include "status.h"

namespace watcher {
namespace {

constexpr const char *TAG = "watcher";

StaticSemaphore_t g_lock_storage;
SemaphoreHandle_t g_lock = nullptr;

Status g_status;
bool g_started = false;

bool g_subscribed = false;
uint16_t g_subscribed_at = 0;

// Parsed on the bus task, so it is a static rather than 700 bytes of somebody
// else's stack (§10.14.1) — the same call `responder.cpp` makes about the request
// it parses there.
ui::Limits g_incoming;

void Lock() { xSemaphoreTake(g_lock, portMAX_DELAY); }
void Unlock() { xSemaphoreGive(g_lock); }

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

    screens::ShowLimits(g_incoming);
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
    if (g_subscribed && (!connected || bus.connects != g_subscribed_at)) {
        if (connected) {
            nats::Unsubscribe(protocol::kStatusSubject);
        }
        g_subscribed = false;
        ESP_LOGI(TAG, "no longer watching %s", protocol::kStatusSubject);
    }

    if (!g_subscribed && connected) {
        // **No queue group** (§10.5, §10.8.3): a broadcast current value is meant
        // to reach every subscriber, and joining a group would mean taking it from
        // `approver-web` and whoever else is watching.
        if (nats::Subscribe(protocol::kStatusSubject, nullptr, &OnStatus, nullptr) == ESP_OK) {
            g_subscribed = true;
            g_subscribed_at = bus.connects;
            ESP_LOGI(TAG, "watching %s", protocol::kStatusSubject);
        }
    }

    Lock();
    g_status.subscribed = g_subscribed;
    g_status.blocked_by = connected ? Blocker::kNone : Blocker::kNoBus;
    Unlock();
}

Status Get() {
    if (!g_started) {
        return Status{};
    }
    Lock();
    const Status copy = g_status;
    Unlock();
    return copy;
}

}  // namespace watcher
