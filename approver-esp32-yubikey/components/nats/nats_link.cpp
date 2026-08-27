#include "nats_link.h"

#include <cstdio>
#include <cstring>

#include "config.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "wifi_manager.h"

namespace nats {

namespace {

constexpr const char *TAG = "natslink";

// **8 KB, which is twice what every other task in this firmware gets, and the
// board is what decided it.** 4 KB — the Wi-Fi manager's number and the
// clock's — panicked with a stack protection fault the first time a server
// actually answered:
//
//     Guru Meditation Error: Core 0 panic'ed (Stack protection fault)
//     Detected in task "nats"
//
// It is the library's frame, not ours. `send_connect()` declares two
// `char[NATS_MAX_CREDENTIAL_LEN * 2 + 1]` buffers for escaping a username and
// a password — 4 KB of stack in one function, reserved whether or not the
// branch that uses them is taken, and this device sends no credentials at all
// (§10.3). On top of that go cJSON's recursion over the server's `INFO` and a
// 2 KB `snprintf`.
//
// The measured cost of the number is printed by `nats` as the low-water mark,
// so it is a fact rather than a guess — the rule §10.14.1 states about the
// heap, applied to a stack.
constexpr uint32_t kStackBytes = kTaskStackBytes;

StackType_t task_stack[kStackBytes];
StaticTask_t task_storage;
TaskHandle_t task_handle = nullptr;

// **Two locks, and they are not the same lock.**
//
//   * `wire_lock` guards the client's *lifetime* — it is held while the object
//     is built or destroyed, and by every caller that publishes through it.
//     The library has its own mutexes for the socket itself, so this one is
//     about the object existing, not about the bytes;
//   * `state_lock` guards the policy and the snapshot. It is never held across
//     anything that blocks, which is what lets `nats` answer instantly while a
//     connect attempt is five seconds into its timeout.
//
// One lock for both would mean the console hanging for the length of a connect
// to print a status line — the thing §10.7 says a console must never look like.
StaticSemaphore_t wire_lock_storage;
SemaphoreHandle_t wire_lock = nullptr;
StaticSemaphore_t state_lock_storage;
SemaphoreHandle_t state_lock = nullptr;

Bus bus;
LinkPolicy policy;
Status snapshot;
bool started = false;
bool desired = true;
esp_err_t last_error = ESP_OK;

// Where the configured address parses to, and whether it parsed at all.
// Guarded by `state_lock`; the task copies it before it opens anything.
Endpoint endpoint = {};
bool endpoint_valid = false;

// Fast enough that a permission request does not sit in a socket buffer while
// the operator waits — what this feeds here is the white light and the key's own
// blink (§10.17), where the sibling board's §10.8.4 fed a screen — slow enough to be
// free: `Bus::Process` is a `select` with a zero timeout and returns at once
// when nothing has arrived. The Wi-Fi manager's 200 ms is the right number for
// a radio and the wrong one for a request card.
constexpr uint32_t kPollMs = 50;

// **Our own bound on somebody else's wait** (§10.5). The library's `connect`
// takes a timeout, and this is what it is: long enough for a DNS lookup and a
// TCP handshake on a busy home network, short enough that a device pointed at
// an address with nothing on it is not sitting in this call for a minute.
constexpr uint32_t kConnectMs = 5000;

uint32_t NowMs() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }

// Is there something to connect *through*? A client link with an address, and
// nothing else — see the header on why the internet check has no vote.
bool NetworkIsUp() {
    if (!wifimgr::Ready()) {
        return false;
    }
    const wifimgr::Snapshot radio = wifimgr::Get();
    return radio.radio.mode == wifi::Mode::kClient &&
           radio.radio.link == wifi::Link::kConnected && radio.radio.ip != 0;
}

// Reads `nats.url` into `endpoint`. Called under `state_lock`.
// **An address that will not parse is "off", loudly** — the same call §10.8.2
// makes about a misspelled zone: guessing at it would be a device quietly
// connecting somewhere other than where the file says.
bool ReadEndpoint(Endpoint *out) {
    const char *url = config::Get().nats.url;
    if (url[0] == '\0') {
        return false;
    }
    if (!ParseUrl(url, out)) {
        ESP_LOGW(TAG, "nats.url is '%s', which is not an address this can use", url);
        return false;
    }
    return true;
}

LinkSettings SettingsFromState() {
    LinkSettings settings;
    settings.enabled = desired && endpoint_valid;
    return settings;
}

// Composed under `state_lock` at the end of every pass, so that `Get()` is a
// copy and never a call into the client.
void UpdateSnapshot(uint32_t now_ms, bool network, const Counters &counters,
                    size_t subscriptions) {
    snapshot.ready = started;
    snapshot.wanted = desired;
    snapshot.configured = endpoint_valid;
    snapshot.endpoint = endpoint;
    snapshot.state = policy.CurrentState();
    snapshot.network = network;
    snapshot.next_attempt_ms = policy.NextAttemptInMs(now_ms);
    snapshot.connected_for_ms = policy.ConnectedForMs(now_ms);
    snapshot.connects = policy.Connects();
    snapshot.drops = policy.Drops();
    snapshot.failures = policy.Failures();
    snapshot.last_error = last_error;
    snapshot.counters = counters;
    snapshot.subscriptions = subscriptions;
    // Called from the task itself, so `nullptr` is this task and the number is
    // the one that matters — how close the library's frames have come to the
    // end of the array above.
    snapshot.stack_low_water = static_cast<uint32_t>(uxTaskGetStackHighWaterMark(nullptr));
}

// One attempt: make sure the client points at the address the file names, then
// open a socket. Runs in the task and nowhere else.
//
// **It hands back the address it actually used**, which is not always the one
// configured: an operator can retype `nats url` while this is five seconds
// into a connect, and a log line naming the new server for an attempt against
// the old one is a log line that sends somebody hunting the wrong fault. The
// board printed exactly that pair once.
esp_err_t OpenAndConnect(Endpoint *used) {
    xSemaphoreTake(state_lock, portMAX_DELAY);
    const Endpoint target = endpoint;
    xSemaphoreGive(state_lock);
    *used = target;

    xSemaphoreTake(wire_lock, portMAX_DELAY);
    // `Open` is a no-op when the client already points here, and a teardown
    // plus a rebuild when it does not — the library takes its endpoint at
    // construction, which is the whole reason this is a call rather than a
    // field.
    esp_err_t err = bus.Open(target);
    xSemaphoreGive(wire_lock);
    if (err != ESP_OK) {
        return err;
    }

    // **Outside the lock**, because it blocks for up to `kConnectMs` and the
    // console has to stay answerable. Nothing else destroys the client — that
    // happens in this task too.
    return bus.Connect(kConnectMs);
}

void Task(void *) {
    for (;;) {
        const uint32_t now = NowMs();
        const bool network = NetworkIsUp();
        const bool socket_up = bus.Connected();

        xSemaphoreTake(state_lock, portMAX_DELAY);
        policy.OnNetwork(network, now);
        if (policy.Connected() && !socket_up) {
            // The server closed it, or the library gave up on it. Not a
            // refusal (§10.5) — the address was right a moment ago.
            policy.OnDropped(now);
        }
        const Action action = policy.Tick(now);
        const bool switched_off = policy.CurrentState() == State::kOff;
        xSemaphoreGive(state_lock);

        if (action == Action::kConnect) {
            Endpoint tried = {};
            const esp_err_t err = OpenAndConnect(&tried);
            const uint32_t settled = NowMs();
            xSemaphoreTake(state_lock, portMAX_DELAY);
            last_error = err;
            policy.OnResult(err == ESP_OK, settled);
            xSemaphoreGive(state_lock);
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "connected to %s:%u", tried.host,
                         static_cast<unsigned>(tried.port));
            } else {
                ESP_LOGW(TAG, "%s:%u did not answer: %s", tried.host,
                         static_cast<unsigned>(tried.port), esp_err_to_name(err));
            }
        } else if (action == Action::kDisconnect) {
            xSemaphoreTake(wire_lock, portMAX_DELAY);
            if (switched_off) {
                // Off should cost nothing: the client goes, and with it the
                // library's mutexes and whatever it was holding.
                bus.Close();
            } else {
                // The network went, or the address changed. The socket goes
                // and the client stays — the library restores its own
                // subscriptions when the socket comes back, and losing them on
                // every Wi-Fi blip would be a responder that stops answering
                // without ever saying so.
                bus.Disconnect();
            }
            xSemaphoreGive(wire_lock);
            ESP_LOGI(TAG, "disconnected");
        }

        // The pump. Not under `wire_lock`: the library's own mutexes cover a
        // publish arriving from the console mid-read, and holding this one
        // here would block every caller for the length of a dispatch.
        bus.Process();

        const Counters counters = bus.Counts();
        const size_t subscriptions = bus.SubscriptionCount();
        xSemaphoreTake(state_lock, portMAX_DELAY);
        UpdateSnapshot(NowMs(), network, counters, subscriptions);
        xSemaphoreGive(state_lock);

        vTaskDelay(pdMS_TO_TICKS(kPollMs));
    }
}

}  // namespace

esp_err_t Init() {
    if (started) {
        return ESP_OK;
    }

    wire_lock = xSemaphoreCreateMutexStatic(&wire_lock_storage);
    state_lock = xSemaphoreCreateMutexStatic(&state_lock_storage);
    if (wire_lock == nullptr || state_lock == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    endpoint_valid = ReadEndpoint(&endpoint);
    policy.Configure(SettingsFromState(), NowMs());

    task_handle =
        xTaskCreateStatic(&Task, "nats", kStackBytes, nullptr, 4, task_stack, &task_storage);
    if (task_handle == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    started = true;

    if (!endpoint_valid) {
        ESP_LOGI(TAG, "no bus address set; nothing will be connected");
    } else {
        ESP_LOGI(TAG, "bus is %s:%u, connecting as soon as there is a network", endpoint.host,
                 static_cast<unsigned>(endpoint.port));
    }
    return ESP_OK;
}

bool Ready() { return started; }

void Apply() {
    if (!started) {
        return;
    }
    xSemaphoreTake(state_lock, portMAX_DELAY);
    Endpoint fresh = {};
    const bool valid = ReadEndpoint(&fresh);
    const bool moved = valid != endpoint_valid || (valid && !Same(fresh, endpoint));
    endpoint = valid ? fresh : Endpoint{};
    endpoint_valid = valid;
    policy.Configure(SettingsFromState(), NowMs());
    if (moved) {
        // The connection that is up is to the wrong server. Nothing else here
        // touches it — a re-read that changed nothing must not cost a
        // reconnect, which is exactly what `wifi check` taught (§10.9).
        policy.Restart(NowMs());
    }
    xSemaphoreGive(state_lock);
}

void SetDesired(bool on) {
    if (!started) {
        return;
    }
    xSemaphoreTake(state_lock, portMAX_DELAY);
    desired = on;
    policy.Configure(SettingsFromState(), NowMs());
    xSemaphoreGive(state_lock);
}

bool Desired() { return desired; }

void ConnectNow() {
    if (!started) {
        return;
    }
    xSemaphoreTake(state_lock, portMAX_DELAY);
    policy.ConnectNow(NowMs());
    xSemaphoreGive(state_lock);
}

void Restart() {
    if (!started) {
        return;
    }
    xSemaphoreTake(state_lock, portMAX_DELAY);
    policy.Restart(NowMs());
    xSemaphoreGive(state_lock);
}

Status Get() {
    Status copy;
    if (!started) {
        return copy;
    }
    xSemaphoreTake(state_lock, portMAX_DELAY);
    copy = snapshot;
    // **The three that are settings rather than state are taken live**, and
    // the board is what asked for it: a connect blocks the task for up to five
    // seconds, so a `nats` typed straight after `nats url` printed the address
    // the device was *leaving* under a config line naming the one it was going
    // to. What was asked for changes the moment it is asked for; only what is
    // happening has to wait for the task to notice.
    copy.wanted = desired;
    copy.configured = endpoint_valid;
    copy.endpoint = endpoint;
    xSemaphoreGive(state_lock);
    return copy;
}

esp_err_t Publish(const char *subject, const char *payload, const char *reply) {
    if (!started) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(wire_lock, portMAX_DELAY);
    const esp_err_t err = bus.Publish(subject, payload, reply);
    xSemaphoreGive(wire_lock);
    return err;
}

esp_err_t Subscribe(const char *subject, const char *queue, Handler handler, void *user) {
    if (!started) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(wire_lock, portMAX_DELAY);
    const esp_err_t err = bus.Subscribe(subject, queue, handler, user);
    xSemaphoreGive(wire_lock);
    return err;
}

esp_err_t Unsubscribe(const char *subject) {
    if (!started) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(wire_lock, portMAX_DELAY);
    const esp_err_t err = bus.Unsubscribe(subject);
    xSemaphoreGive(wire_lock);
    return err;
}

bool Flush(uint32_t timeout_ms) {
    if (!started) {
        return false;
    }
    xSemaphoreTake(wire_lock, portMAX_DELAY);
    const bool ok = bus.Flush(timeout_ms);
    xSemaphoreGive(wire_lock);
    return ok;
}

bool SubscriptionAt(size_t index, SubscriptionRow *out) {
    if (!started || out == nullptr) {
        return false;
    }
    SubscriptionInfo info = {};
    xSemaphoreTake(wire_lock, portMAX_DELAY);
    const bool found = bus.SubscriptionAt(index, &info);
    if (found) {
        // Copied while the lock is held: the pointers belong to a table the
        // task can rebuild the moment it is let go.
        snprintf(out->subject, sizeof(out->subject), "%s", info.subject);
        snprintf(out->queue, sizeof(out->queue), "%s", info.queue);
        out->sid = info.sid;
    }
    xSemaphoreGive(wire_lock);
    return found;
}

}  // namespace nats
