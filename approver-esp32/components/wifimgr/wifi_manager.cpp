#include "wifi_manager.h"

#include <cstring>

#include "config.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "ping/ping_sock.h"

namespace wifimgr {

namespace {

constexpr const char *TAG = "wifimgr";

// The one place allowed to know both halves (see `wifi_policy.h`): the policy
// counts networks and the config stores them, and a mismatch would be a
// bitmask indexing past the array it describes.
static_assert(kMaxNetworks == config::kMaxNetworks,
              "the policy's network bitmask and config.json's list must be the same length");
static_assert(wifi::kSsidSize == config::kSsidSize, "one SSID field, two headers");
static_assert(wifi::kPasswordSize == config::kPasswordSize, "one password field, two headers");

// 4 KB, in bytes: ESP-IDF's `StackType_t` is a byte, so the array length *is*
// the depth. The task logs, calls into `esp_wifi` and does nothing recursive.
constexpr uint32_t kStackBytes = 4096;

StackType_t task_stack[kStackBytes];
StaticTask_t task_storage;
TaskHandle_t task_handle = nullptr;

StaticSemaphore_t lock_storage;
SemaphoreHandle_t lock = nullptr;

wifi::Radio radio;
Policy policy;
Reachability reach;

static_assert(kMaxProbeTargets == config::kMaxProbeTargets,
              "the probe list and config.json's must be the same length");

// --- the ICMP echo itself ---------------------------------------------------
//
// `esp_ping` runs each session on a task of its own and answers through
// callbacks, so this is a small mailbox between that task and ours. The
// decisions — when to ask, which address, what a failed round means — are all
// in `reachability.h` and none of them are here.

struct PingMailbox {
    volatile bool done;
    volatile bool replied;
};
PingMailbox ping_mail = {};
esp_ping_handle_t ping_handle = nullptr;
uint32_t ping_started_ms = 0;
uint32_t ping_deadline_ms = 0;

void OnPingReply(esp_ping_handle_t, void *) { ping_mail.replied = true; }
void OnPingEnd(esp_ping_handle_t, void *) { ping_mail.done = true; }

void ClosePing() {
    if (ping_handle == nullptr) {
        return;
    }
    esp_ping_stop(ping_handle);
    esp_ping_delete_session(ping_handle);
    ping_handle = nullptr;
}

// One echo, one answer. **A session per probe**, created and deleted around
// each one: `esp_ping` has no way to retarget a live session, and once a
// minute is nowhere near often enough for the task churn to matter.
bool StartPing(uint32_t address, uint32_t timeout_ms, uint32_t now_ms) {
    if (ping_handle != nullptr) {
        return false;
    }
    esp_ping_config_t config = ESP_PING_DEFAULT_CONFIG();
    // One packet, not the default five: this is a yes/no question about
    // whether anything is out there, not a link-quality measurement.
    config.count = 1;
    config.timeout_ms = timeout_ms;
    config.data_size = 32;
    config.task_stack_size = 3072;
    // lwIP's `ip_addr_t`, so `IPADDR_TYPE_V4` — not `esp_netif`'s
    // `ESP_IPADDR_TYPE_V4`, which is the same idea one layer up and does not
    // name this type's constants.
    config.target_addr.type = IPADDR_TYPE_V4;
    config.target_addr.u_addr.ip4.addr = address;

    esp_ping_callbacks_t callbacks = {};
    callbacks.on_ping_success = &OnPingReply;
    callbacks.on_ping_end = &OnPingEnd;

    ping_mail.done = false;
    ping_mail.replied = false;
    if (esp_ping_new_session(&config, &callbacks, &ping_handle) != ESP_OK) {
        ping_handle = nullptr;
        return false;
    }
    if (esp_ping_start(ping_handle) != ESP_OK) {
        ClosePing();
        return false;
    }
    ping_started_ms = now_ms;
    // **Our own deadline on top of the library's.** §10.5's rule about
    // bounding every read, applied to somebody else's task: a session that
    // never calls back would otherwise leave the check waiting forever and the
    // state stuck at whatever it last was.
    ping_deadline_ms = timeout_ms + 3000;
    return true;
}

// What the last pass saw, so an edge is reported once. The radio latches
// `kFailed` until the next `Start…` precisely so this can be a comparison
// rather than a queue.
bool reported_connected = false;
bool reported_failed = false;

// An action the radio was too busy to take (a scan is running). Kept rather
// than dropped: dropping it would leave the policy in `kConnecting` against a
// radio that was never asked, and the only thing that would rescue it is the
// connect timeout.
Action pending = Action::kNone;

bool radio_ready = false;
esp_err_t radio_error = ESP_OK;
bool started = false;

uint32_t NowMs() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }

// The driver's three answers, in the policy's vocabulary. Two enums rather
// than one shared header, because sharing it would mean the policy including
// something from the driver — and the policy including nothing is what makes
// it testable on the host (§10.11).
Failure Translate(wifi::Failure failure) {
    switch (failure) {
        case wifi::Failure::kAuth:
            return Failure::kAuth;
        case wifi::Failure::kNotFound:
            return Failure::kNotFound;
        case wifi::Failure::kOther:
            return Failure::kOther;
        case wifi::Failure::kNone:
            break;
    }
    return Failure::kOther;
}

// `config.json` keeps addresses as text (so a `cat` shows what was typed) and
// the driver takes them binary. This is the one place the two meet, and it is
// in the manager for the same reason `Translate` above is: the driver has
// never heard of a config file and the policy has never heard of an address.
//
// Anything that does not parse has already been dropped at load (§10.15's
// `CopyAddress`), so a field that is empty here is one the file did not have
// or the parser refused — either way the answer is the same: not set.
wifi::StaticIp AddressFor(const config::Network &network) {
    wifi::StaticIp ip;
    if (!network.ip.enabled) {
        return ip;
    }
    // Belt and braces: `config` will not enable a static block whose three
    // required fields are not all addresses, so a failure here means the two
    // disagree — and going out with half an address is the one outcome worth
    // ruling out twice.
    if (!config::ParseIpv4(network.ip.address, &ip.address) ||
        !config::ParseIpv4(network.ip.netmask, &ip.netmask) ||
        !config::ParseIpv4(network.ip.gateway, &ip.gateway)) {
        ESP_LOGE(TAG, "'%s' has a static address that will not parse; using DHCP", network.ssid);
        return wifi::StaticIp{};
    }
    config::ParseIpv4(network.ip.dns1, &ip.dns1);  // optional; 0 stays 0
    config::ParseIpv4(network.ip.dns2, &ip.dns2);
    ip.enabled = true;
    return ip;
}

ProbeSettings ProbesFromConfig() {
    const config::InternetCheck &net = config::Get().internet;
    ProbeSettings settings;
    settings.enabled = net.check;
    settings.target_count = net.target_count;
    settings.interval_ms = static_cast<uint32_t>(net.interval_seconds) * 1000u;
    settings.failures_before_offline = net.failures_before_offline;
    return settings;
}

Settings SettingsFromConfig() {
    const config::Wifi &wifi_config = config::Get().wifi;
    Settings settings;
    settings.network_count = wifi_config.network_count;
    settings.rounds_before_ap = wifi_config.rounds_before_ap;
    settings.ap_window_ms = static_cast<uint32_t>(wifi_config.ap_window_seconds) * 1000u;
    // The rest are not in the file. They are the shape of §10.9 rather than a
    // preference — a device whose connect timeout is operator-settable is a
    // device with one more way to be configured into never working.
    return settings;
}

// Brings the radio up the first time something actually wants it. Returns
// false once and logs; after that it is quiet, because a radio that will not
// initialise will not initialise on the next pass either.
bool EnsureRadio() {
    if (radio_ready) {
        return true;
    }
    if (radio_error != ESP_OK) {
        return false;
    }
    const esp_err_t err = radio.Init();
    if (err != ESP_OK) {
        radio_error = err;
        ESP_LOGE(TAG, "the radio would not start (%s); wi-fi is off", esp_err_to_name(err));
        return false;
    }
    radio_ready = true;
    return true;
}

// Carries out one policy decision. False means "not now, ask again" — which is
// only ever a scan holding the radio, and is deliberately not a failure: a
// failure here would spend one of the rounds §10.9 counts on something that
// was never tried.
bool ApplyAction(Action action, uint32_t now_ms) {
    switch (action) {
        case Action::kNone:
            return true;

        case Action::kStop:
            if (radio_ready) {
                radio.Stop();
            }
            reported_connected = false;
            reported_failed = false;
            return true;

        case Action::kStartAp: {
            if (!EnsureRadio()) {
                return true;
            }
            const config::Wifi &wifi_config = config::Get().wifi;
            const esp_err_t err =
                radio.StartAp(wifi_config.ap_ssid, wifi_config.ap_password, wifi_config.ap_channel);
            if (err == ESP_ERR_INVALID_STATE) {
                return false;
            }
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "access point '%s': %s", wifi_config.ap_ssid, esp_err_to_name(err));
            }
            reported_connected = false;
            reported_failed = false;
            return true;
        }

        case Action::kStartClient: {
            if (!EnsureRadio()) {
                return true;
            }
            const config::Wifi &wifi_config = config::Get().wifi;
            const uint8_t index = policy.Network();
            if (index >= wifi_config.network_count) {
                // The list shrank under the policy. Report it as a failure so
                // the cycle moves on rather than stalling on an index that no
                // longer names anything.
                policy.OnFailed(Failure::kOther, now_ms);
                return true;
            }
            const config::Network &network = wifi_config.networks[index];
            const wifi::StaticIp ip = AddressFor(network);
            const esp_err_t err = radio.StartClient(network.ssid, network.password, &ip);
            if (err == ESP_ERR_INVALID_STATE) {
                // Busy — a scan has the radio. **Nothing is logged on this
                // path**, and that is a fix rather than an omission: the retry
                // comes round every `kPollMs`, so a log line here printed
                // "joining 'x'" five times for one join while a scan ran.
                return false;
            }
            // **The SSID is logged and the password never is** (§10.15): a
            // secret from the moment it is typed, and a boot log is exactly
            // the place it must not turn up.
            ESP_LOGI(TAG, "joining '%s' (network %u, round %u, %s)", network.ssid,
                     static_cast<unsigned>(index + 1), static_cast<unsigned>(policy.Round() + 1),
                     ip.enabled ? network.ip.address : "dhcp");
            reported_connected = false;
            reported_failed = false;
            if (err != ESP_OK) {
                policy.OnFailed(Failure::kOther, now_ms);
            }
            return true;
        }
    }
    return true;
}

// The internet check (§10.9), driven from the same 200 ms pass as everything
// else. Three steps, and each is a couple of lines because the thinking is
// next door in `reachability.h`.
void PumpReachability(uint32_t now, bool link_is_up) {
    if (link_is_up) {
        reach.LinkUp(now);
    } else {
        reach.LinkDown(now);
        // A session outstanding when the link went is a session whose answer
        // is about nothing.
        if (ping_handle != nullptr) {
            ClosePing();
        }
        return;
    }

    if (ping_handle != nullptr) {
        if (ping_mail.done) {
            const bool replied = ping_mail.replied;
            ClosePing();
            reach.OnResult(replied, now);
        } else if (now - ping_started_ms >= ping_deadline_ms) {
            // The library did not call back. Count it as a failure rather than
            // waiting: an unanswered question is the same as a no here, and
            // hanging on it would freeze the check for good.
            ESP_LOGW(TAG, "ping session did not finish; treating it as no answer");
            ClosePing();
            reach.OnResult(false, now);
        }
        return;
    }

    if (reach.Tick(now) != Probe::kSend) {
        return;
    }

    const config::InternetCheck &net = config::Get().internet;
    const uint8_t index = reach.Target();
    uint32_t address = 0;
    if (index >= net.target_count || !config::ParseIpv4(net.targets[index], &address)) {
        // The list changed under the round, or a target that `config` should
        // have refused got through. Either way this probe cannot be sent.
        reach.OnResult(false, now);
        return;
    }
    if (!StartPing(address, net.timeout_ms, now)) {
        reach.OnResult(false, now);
    }
}

void Pump() {
    const uint32_t now = NowMs();

    // What the radio has to say, turned into events. Each edge is reported
    // once: `reported_*` is cleared by every `Start…` above, which is the only
    // thing that can begin a new attempt.
    bool link_is_up = false;
    if (radio_ready) {
        const wifi::Status status = radio.Get();
        policy.OnApClients(status.clients, now);

        if (status.link == wifi::Link::kConnected && !reported_connected) {
            reported_connected = true;
            policy.OnConnected(now);
        } else if (status.link == wifi::Link::kFailed && !reported_failed) {
            reported_failed = true;
            policy.OnFailed(Translate(status.failure), now);
        }

        // **Connected as a client and holding an address**, which is the only
        // state in which asking about the internet means anything: an access
        // point has no uplink of its own to test.
        link_is_up = status.mode == wifi::Mode::kClient &&
                     status.link == wifi::Link::kConnected && status.ip != 0;
    }
    PumpReachability(now, link_is_up);

    // The desired mode is read from the file on every pass — `SetDesired` is
    // idempotent, so this costs nothing and means an edit takes effect without
    // anybody having to remember to tell the manager about it.
    policy.SetDesired(DesiredFromConfig(), now);

    if (pending == Action::kNone) {
        pending = policy.Tick(now);
    }
    if (pending != Action::kNone && ApplyAction(pending, now)) {
        pending = Action::kNone;
    }
}

void Task(void *) {
    for (;;) {
        if (xSemaphoreTake(lock, pdMS_TO_TICKS(kPollMs)) == pdTRUE) {
            Pump();
            xSemaphoreGive(lock);
        }
        vTaskDelay(pdMS_TO_TICKS(kPollMs));
    }
}

}  // namespace

Desired DesiredFromConfig() {
    const config::Wifi &wifi_config = config::Get().wifi;
    if (!wifi_config.active) {
        return Desired::kOff;
    }
    return wifi_config.mode == config::WifiMode::kAp ? Desired::kAp : Desired::kClient;
}

esp_err_t Init() {
    if (started) {
        return ESP_OK;
    }

    lock = xSemaphoreCreateMutexStatic(&lock_storage);
    if (lock == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    policy.Configure(SettingsFromConfig(), NowMs());
    reach.Configure(ProbesFromConfig(), NowMs());

    task_handle = xTaskCreateStatic(&Task, "wifimgr", kStackBytes, nullptr, 4, task_stack,
                                    &task_storage);
    if (task_handle == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    started = true;

    const Desired desired = DesiredFromConfig();
    ESP_LOGI(TAG, "manager up; config asks for %s, %u network(s)", Name(desired),
             static_cast<unsigned>(config::Get().wifi.network_count));
    return ESP_OK;
}

bool Ready() { return started; }

void Apply() {
    if (!started) {
        return;
    }
    xSemaphoreTake(lock, portMAX_DELAY);
    pending = Action::kNone;
    policy.Configure(SettingsFromConfig(), NowMs());
    policy.SetDesired(DesiredFromConfig(), NowMs());
    reach.Configure(ProbesFromConfig(), NowMs());
    xSemaphoreGive(lock);
}

void ApplyInternetCheck() {
    if (!started) {
        return;
    }
    xSemaphoreTake(lock, portMAX_DELAY);
    // Deliberately *not* `policy.Configure`: the ping list and the network
    // list are different settings, and reconfiguring the second because the
    // first changed is how `wifi check 8.8.8.8` came to tear down a working
    // connection and rejoin from the top of the list.
    reach.Configure(ProbesFromConfig(), NowMs());
    xSemaphoreGive(lock);
}

void CheckInternetNow() {
    if (!started) {
        return;
    }
    xSemaphoreTake(lock, portMAX_DELAY);
    reach.ProbeNow(NowMs());
    xSemaphoreGive(lock);
}

void SetDesired(Desired desired) {
    if (!started) {
        return;
    }
    xSemaphoreTake(lock, portMAX_DELAY);
    // The action in flight belonged to the mode being left.
    pending = Action::kNone;
    policy.SetDesired(desired, NowMs());
    xSemaphoreGive(lock);
}

Snapshot Get() {
    Snapshot snapshot;
    if (!started) {
        return snapshot;
    }

    const uint32_t now = NowMs();
    xSemaphoreTake(lock, portMAX_DELAY);
    snapshot.desired = policy.GetDesired();
    snapshot.state = policy.GetState();
    snapshot.failure = policy.LastFailure();
    snapshot.network = policy.Network();
    snapshot.round = policy.Round();
    for (uint8_t i = 0; i < kMaxNetworks; ++i) {
        snapshot.auth_failed[i] = policy.AuthFailed(i);
    }
    snapshot.ap_window_remaining_ms = policy.ApWindowRemainingMs(now);
    snapshot.wait_remaining_ms = policy.WaitRemainingMs(now);
    snapshot.radio_ready = radio_ready;
    snapshot.radio_error = radio_error;
    snapshot.internet = reach.State();
    snapshot.internet_failed_rounds = reach.FailedRounds();
    snapshot.internet_last_ok_ms = reach.SinceLastSuccessMs(now);
    snapshot.internet_next_probe_ms = reach.NextProbeInMs(now);
    snapshot.internet_target = reach.Target();
    xSemaphoreGive(lock);

    // Outside the lock: `Radio::Get` takes its own, and holding two locks in
    // one order here and the other order anywhere else is the shape of a
    // deadlock nobody finds twice.
    if (radio_ready) {
        snapshot.radio = radio.Get();
    }
    return snapshot;
}

esp_err_t Scan(wifi::ScanResult *out, size_t capacity, size_t *found) {
    if (!started) {
        return ESP_ERR_INVALID_STATE;
    }

    // **A scan is the one thing allowed to wake a radio nobody asked for.**
    // Everywhere else the radio comes up because `config.json` wants a
    // network; here somebody is asking what is out there, which is exactly the
    // question you have with the radio off and no network that works. The
    // driver puts back down whatever it had to raise — the station, and the
    // Wi-Fi stack with it if that was down too — so this borrows the stack's
    // heap for the two seconds it runs rather than keeping it.
    //
    // What `EnsureRadio` takes and does not give back is the once-ever half:
    // NVS, `esp_netif` and the event handlers, a few kilobytes.
    //
    // The lock is held only across the bring-up flag, never across the scan
    // itself: that blocks for a second or two, and the manager task has a poll
    // to keep making.
    xSemaphoreTake(lock, portMAX_DELAY);
    const bool have_radio = EnsureRadio();
    xSemaphoreGive(lock);
    if (!have_radio) {
        return radio_error != ESP_OK ? radio_error : ESP_ERR_INVALID_STATE;
    }
    // **The manager's lock is not taken.** A scan blocks for a second or two
    // and the radio has its own lock; making the manager task wait that long
    // for its poll would be a self-inflicted stall. What the manager does see
    // is `StartClient` refusing with `ESP_ERR_INVALID_STATE` while the scan
    // runs, which `ApplyAction` treats as "ask again" rather than as a
    // failure.
    return radio.Scan(out, capacity, found);
}

}  // namespace wifimgr
