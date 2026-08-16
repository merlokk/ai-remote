#include "wifi_radio.h"

#include <cstdio>
#include <cstring>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "soc/soc_caps.h"

namespace wifi {

namespace {

constexpr const char *TAG = "wifi";

// The scan's landing area. Static, because nothing here allocates (§10.14.1),
// and in the `.cpp` because keeping `wifi_ap_record_t` out of the header is
// what lets `wifi_radio.h` be read without ESP-IDF in the picture. About
// 1.5 KB, present whether or not anybody scans — which is the honest cost of
// the rule and is why sixteen and not sixty-four.
wifi_ap_record_t scan_records[kMaxScanResults];

// Copies into a fixed field, refusing rather than truncating — the call
// `config.cpp` makes about the same two strings, for the same reason: half an
// SSID fails to connect and gives no hint which half is being used.
bool CopyField(char *out, size_t capacity, const char *in) {
    const size_t length = in == nullptr ? 0 : strlen(in);
    if (length >= capacity) {
        return false;
    }
    memcpy(out, in == nullptr ? "" : in, length + 1);
    return true;
}

}  // namespace

// --- classification ---------------------------------------------------------

Failure Radio::Classify(uint8_t reason) {
    switch (reason) {
        // The password. `4WAY_HANDSHAKE_TIMEOUT` is what this chip reports for
        // a wrong WPA2 passphrase far more often than `AUTH_FAIL` — the AP
        // does not say "wrong", it simply stops answering the handshake.
        case WIFI_REASON_MIC_FAILURE:
        case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
        case WIFI_REASON_AUTH_FAIL:
        case WIFI_REASON_HANDSHAKE_TIMEOUT:
            return Failure::kAuth;

        // Nothing on the air with that name. The three qualified variants are
        // here too: they mean the network was not found *under the constraints
        // we set*, which from the operator's side is the same problem and
        // never a password to retype. §10.9 asks for that to be reported
        // differently from a refusal, and this is the line that does it.
        case WIFI_REASON_NO_AP_FOUND:
        case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
        case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
        case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:
            return Failure::kNotFound;

        default:
            return Failure::kOther;
    }
}

const char *Radio::Name(Mode mode) {
    switch (mode) {
        case Mode::kOff:
            return "off";
        case Mode::kClient:
            return "client";
        case Mode::kAp:
            return "ap";
    }
    return "?";
}

const char *Radio::Name(Link link) {
    switch (link) {
        case Link::kIdle:
            return "idle";
        case Link::kConnecting:
            return "connecting";
        case Link::kConnected:
            return "connected";
        case Link::kFailed:
            return "failed";
    }
    return "?";
}

const char *Radio::Name(Failure failure) {
    switch (failure) {
        case Failure::kNone:
            return "none";
        case Failure::kAuth:
            return "wrong password";
        case Failure::kNotFound:
            return "no such network";
        case Failure::kOther:
            return "disconnected";
    }
    return "?";
}

// --- bring-up ---------------------------------------------------------------

esp_err_t Radio::Init() {
    if (ready_) {
        return ESP_OK;
    }

    lock_ = xSemaphoreCreateMutexStatic(&lock_storage_);
    if (lock_ == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    // **NVS holds nothing of ours** (§10.15) — it is initialised because
    // `esp_wifi` keeps its calibration and PHY data there and will not start
    // without it. A partition that is full or was written by a newer format is
    // erased rather than treated as fatal: what is in it is regenerable, and a
    // device that will not bring up its radio is worse than one that
    // recalibrates.
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "nvs unusable (%s); erasing it", esp_err_to_name(err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_init: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_netif_init();
    if (err != ESP_OK) {
        return err;
    }

    // Somebody else may already own the default loop — `esp_console` does not,
    // but the day something does, that is not an error for this component.
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    // **Registered once and never unregistered**, which is why they are in
    // this half: an event handler is attached to the loop rather than to the
    // Wi-Fi driver, so it survives the stack being torn down and put back up.
    // Registering before there is anything to post events is fine — an event
    // base is a symbol, not a subscription to something running.
    err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &EventTrampoline, this,
                                              nullptr);
    if (err == ESP_OK) {
        err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &EventTrampoline,
                                                  this, nullptr);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "event handlers: %s", esp_err_to_name(err));
        return err;
    }

    ready_ = true;
    ESP_LOGI(TAG, "radio ready (the stack is not up — that is the manager's call)");
    return ESP_OK;
}

esp_err_t Radio::EnsureStack() {
    if (stack_ready_) {
        return ESP_OK;
    }

    // **The netifs are made here and destroyed in `ReleaseStack`**, not kept
    // for the life of the device. `esp_netif_create_default_wifi_sta()` may
    // not be called twice for the same interface, so a stack that comes back
    // up needs them created again rather than reused — which is the whole
    // reason bring-up is in two halves rather than one. They also have to
    // exist *before* `esp_wifi_init`, which is the order every ESP-IDF example
    // uses and the order below.
    sta_netif_ = esp_netif_create_default_wifi_sta();
    ap_netif_ = esp_netif_create_default_wifi_ap();
    if (sta_netif_ == nullptr || ap_netif_ == nullptr) {
        ReleaseStack();
        return ESP_FAIL;
    }

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_wifi_init(&init);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init: %s", esp_err_to_name(err));
        // The netifs are up and the stack is not; leaving them would leak a
        // pair that the next attempt could not create again.
        esp_netif_destroy_default_wifi(sta_netif_);
        esp_netif_destroy_default_wifi(ap_netif_);
        sta_netif_ = nullptr;
        ap_netif_ = nullptr;
        return err;
    }

    // **§10.9, and it is the reason `config.json` is worth anything.** The
    // driver's own credential store would be a second record of what this
    // device knows — one that a config restore does not clear and that nobody
    // can read off a `cat`. There is one record, and it is the file.
    //
    // It has to be set after every `esp_wifi_init`, not once at boot: the
    // setting lives in the stack that was just created.
    err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err != ESP_OK) {
        return err;
    }

    stack_ready_ = true;
    return ESP_OK;
}

void Radio::ReleaseStack() {
    if (started_) {
        esp_wifi_stop();
        started_ = false;
    }
    if (stack_ready_) {
        esp_wifi_deinit();
        stack_ready_ = false;
    }
    // Destroyed in the opposite order to their creation, and only if they were
    // made: `esp_netif_destroy_default_wifi` on a null pointer is not a call
    // worth finding out about.
    if (ap_netif_ != nullptr) {
        esp_netif_destroy_default_wifi(ap_netif_);
        ap_netif_ = nullptr;
    }
    if (sta_netif_ != nullptr) {
        esp_netif_destroy_default_wifi(sta_netif_);
        sta_netif_ = nullptr;
    }
    // Nothing can post an event now, so anything we were waiting to ignore is
    // never coming.
    want_connect_ = false;
    suppress_disconnect_ = 0;
}

// --- what the radio is asked to do ------------------------------------------

esp_err_t Radio::EnsureStarted() {
    if (started_) {
        return ESP_OK;
    }
    const esp_err_t err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start: %s", esp_err_to_name(err));
        return err;
    }
    started_ = true;
    return ESP_OK;
}

void Radio::StopStation() {
    if (!started_) {
        return;
    }
    SuppressNextDisconnect();
    esp_wifi_disconnect();
    esp_wifi_stop();
    started_ = false;
    want_connect_ = false;
}

// One DNS server, skipped when the entry is zero — which is how "not set" is
// spelled and is the one value that could not be a real server.
esp_err_t Radio::SetDns(uint32_t address, int type) {
    if (address == 0) {
        return ESP_OK;
    }
    esp_netif_dns_info_t dns = {};
    dns.ip.type = ESP_IPADDR_TYPE_V4;
    dns.ip.u_addr.ip4.addr = address;
    return esp_netif_set_dns_info(static_cast<esp_netif_t *>(sta_netif_),
                                  static_cast<esp_netif_dns_type_t>(type), &dns);
}

void Radio::ApplyAddressing() {
    auto *netif = static_cast<esp_netif_t *>(sta_netif_);
    if (netif == nullptr) {
        return;
    }

    if (!static_ip_.enabled) {
        // **Put the DHCP client back.** Nothing here starts it — `esp_netif`
        // does, when the interface comes up — but a *previous* network with a
        // fixed address stopped it, and the netif outlives one association.
        // Without this, joining a static network and then a DHCP one gives an
        // interface that never asks for an address and a device that hangs at
        // "connecting" with no reason to show for it.
        const esp_err_t err = esp_netif_dhcpc_start(netif);
        if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED) {
            ESP_LOGW(TAG, "dhcp client would not start: %s", esp_err_to_name(err));
        }
        return;
    }

    // The order is the one the house firmware of §10.14.4 uses and the one
    // ESP-IDF documents: stop asking, then say what the answer is. Setting the
    // address with the client still running is a race against a lease.
    esp_err_t err = esp_netif_dhcpc_stop(netif);
    if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
        ESP_LOGE(TAG, "dhcp client would not stop: %s — staying on DHCP",
                 esp_err_to_name(err));
        return;
    }

    esp_netif_ip_info_t info = {};
    info.ip.addr = static_ip_.address;
    info.netmask.addr = static_ip_.netmask;
    info.gw.addr = static_ip_.gateway;
    err = esp_netif_set_ip_info(netif, &info);
    if (err != ESP_OK) {
        // Nothing to roll back to that is better than this: the interface has
        // no address either way, and the association will time out and be
        // reported as a failure like any other (§10.9).
        ESP_LOGE(TAG, "static address refused: %s", esp_err_to_name(err));
        return;
    }

    if (SetDns(static_ip_.dns1, ESP_NETIF_DNS_MAIN) != ESP_OK ||
        SetDns(static_ip_.dns2, ESP_NETIF_DNS_BACKUP) != ESP_OK) {
        // A name server that would not take is worth one line and not a
        // failed join: this device talks to an address (§10.3), so DNS is a
        // convenience rather than the point.
        ESP_LOGW(TAG, "static dns servers not fully set");
    }

    ESP_LOGI(TAG, "static address " IPSTR "/" IPSTR " gw " IPSTR, IP2STR(&info.ip),
             IP2STR(&info.netmask), IP2STR(&info.gw));

    // **`esp_netif_set_ip_info` on an interface that is already up raises
    // `IP_EVENT_STA_GOT_IP` itself**, so the link still becomes `kConnected`
    // through the one path that does it for DHCP. Nothing special is needed
    // here, and that is worth writing down because it looks like an omission.
}

esp_err_t Radio::StartClient(const char *ssid, const char *password, const StaticIp *ip) {
    if (!ready_) {
        return ESP_ERR_INVALID_STATE;
    }
    if (scanning_) {
        // Refused rather than queued: the caller is a state machine that will
        // ask again on its next pass, and a queued join behind a scan is a
        // join whose credentials may have been edited by the time it runs.
        return ESP_ERR_INVALID_STATE;
    }
    if (ssid == nullptr || ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    wifi_config_t config = {};
    if (!CopyField(reinterpret_cast<char *>(config.sta.ssid), sizeof(config.sta.ssid), ssid) ||
        !CopyField(reinterpret_cast<char *>(config.sta.password), sizeof(config.sta.password),
                   password)) {
        ESP_LOGE(TAG, "ssid or password longer than 802.11 allows");
        return ESP_ERR_INVALID_ARG;
    }

    // **The threshold is left at OPEN, which is not a security decision.** It
    // is the *minimum* authmode this station will accept from an AP; setting
    // it to WPA2 makes an open café network and a WPA3-only router both
    // vanish, and §10.9 wants open, WPA2 and WPA3-SAE all joinable. What
    // actually protects the link is the AP's own security, not our floor.
    //
    // And ESP-IDF does not entirely leave it alone, which is worth knowing
    // before somebody reads the two as contradicting each other. Measured on
    // this board: with a password set it raises the floor itself and says so —
    //
    //   W wifi: Password length matches WPA2 standards, authmode threshold
    //           changes from OPEN to WPA2
    //
    // — so a network we hold a password for will not be joined if it is
    // suddenly open. That is the framework's call and the right one; what this
    // line still buys is the *passwordless* case staying joinable.
    config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    // Retries are the policy's, not the driver's (§10.9): this driver reports
    // one outcome per attempt and lets `wifimgr` decide what it means.
    config.sta.failure_retry_cnt = 0;

    // The stack, if this is the first thing to want it since boot or since the
    // last `Stop`. Validated arguments first, so a typo does not pay for it.
    const esp_err_t stack = EnsureStack();
    if (stack != ESP_OK) {
        return stack;
    }

    // Whatever the station was doing is over. The disconnection this produces
    // is ours, and reporting it as a failure would fail the attempt below
    // before it started.
    if (started_) {
        SuppressNextDisconnect();
        esp_wifi_disconnect();
    }

    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err == ESP_OK) {
        err = esp_wifi_set_config(WIFI_IF_STA, &config);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "station config: %s", esp_err_to_name(err));
        return err;
    }

    // Copied now, applied when the association lands. A pointer kept until
    // then would be a pointer into a network list the console can edit.
    static_ip_ = ip != nullptr ? *ip : StaticIp{};

    {
        Status update = {};
        update.mode = Mode::kClient;
        update.link = Link::kConnecting;
        update.ip_is_static = static_ip_.enabled;
        CopyField(update.ssid, sizeof(update.ssid), ssid);
        xSemaphoreTake(lock_, portMAX_DELAY);
        update.changes = status_.changes + 1;
        status_ = update;
        xSemaphoreGive(lock_);
    }

    if (!started_) {
        // The association is started from the `STA_START` event rather than
        // here: calling `esp_wifi_connect` before the driver has finished
        // starting is the documented way to get `ESP_ERR_WIFI_STATE`.
        want_connect_ = true;
        err = EnsureStarted();
        if (err != ESP_OK) {
            want_connect_ = false;
            SetLink(Link::kFailed, Failure::kOther, 0);
        }
        return err;
    }

    err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "connect to '%s': %s", ssid, esp_err_to_name(err));
        SetLink(Link::kFailed, Failure::kOther, 0);
    }
    return err;
}

esp_err_t Radio::StartAp(const char *ssid, const char *password, uint8_t channel) {
    if (!ready_) {
        return ESP_ERR_INVALID_STATE;
    }
    if (scanning_) {
        return ESP_ERR_INVALID_STATE;
    }
    if (ssid == nullptr || ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    const size_t password_length = password == nullptr ? 0 : strlen(password);
    if (password_length != 0 && password_length < kMinApPasswordLength) {
        // Refused, not downgraded. WPA2 will not take a passphrase this short,
        // and the failure mode of "accept it and open the network instead" is
        // an access point somebody believes is protected.
        ESP_LOGE(TAG, "an AP password must be %u characters or more, or empty for an open network",
                 static_cast<unsigned>(kMinApPasswordLength));
        return ESP_ERR_INVALID_ARG;
    }

    wifi_config_t config = {};
    if (!CopyField(reinterpret_cast<char *>(config.ap.ssid), sizeof(config.ap.ssid), ssid) ||
        !CopyField(reinterpret_cast<char *>(config.ap.password), sizeof(config.ap.password),
                   password)) {
        return ESP_ERR_INVALID_ARG;
    }
    config.ap.ssid_len = static_cast<uint8_t>(strlen(ssid));
    config.ap.channel = channel;
    config.ap.authmode = password_length == 0 ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
    // Four is the whole point of the thing: one operator with one phone or one
    // laptop, and headroom for the second attempt after the first went wrong.
    config.ap.max_connection = 4;
    config.ap.pmf_cfg.required = false;

    const esp_err_t stack = EnsureStack();
    if (stack != ESP_OK) {
        return stack;
    }

    if (started_) {
        SuppressNextDisconnect();
        esp_wifi_disconnect();
    }

    // **APSTA rather than AP**, and the station half is deliberately idle. A
    // scan needs the station interface, and the moment somebody most needs to
    // scan is exactly this one — the fallback AP went up because nothing would
    // have us, and the next thing they will do is look for a network to type a
    // password for. In plain `WIFI_MODE_AP` that scan is not possible at all.
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err == ESP_OK) {
        err = esp_wifi_set_config(WIFI_IF_AP, &config);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ap config: %s", esp_err_to_name(err));
        return err;
    }

    {
        Status update = {};
        update.mode = Mode::kAp;
        update.link = Link::kIdle;
        CopyField(update.ssid, sizeof(update.ssid), ssid);
        xSemaphoreTake(lock_, portMAX_DELAY);
        update.changes = status_.changes + 1;
        status_ = update;
        xSemaphoreGive(lock_);
    }

    err = EnsureStarted();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "access point '%s' up on channel %u, %s", ssid, static_cast<unsigned>(channel),
                 password_length == 0 ? "open" : "wpa2");
    }
    return err;
}

esp_err_t Radio::Stop() {
    if (!ready_ || (!started_ && !stack_ready_)) {
        return ESP_OK;
    }

    // **Off means off, including the heap.** `ReleaseStack` stops the station,
    // deinitialises and destroys the netifs; the ~41 KB the Wi-Fi stack holds
    // goes back to a part that has 512 KB and no way to add more (§10.1).
    if (started_) {
        SuppressNextDisconnect();
        esp_wifi_disconnect();
    }
    ReleaseStack();
    const esp_err_t err = ESP_OK;

    Status update = {};
    xSemaphoreTake(lock_, portMAX_DELAY);
    update.changes = status_.changes + 1;
    status_ = update;
    xSemaphoreGive(lock_);
    return err;
}

Status Radio::Get() const {
    // The signal, read **before** the lock is taken: `esp_wifi_sta_get_ap_info`
    // goes into the Wi-Fi task, and the one rule this lock has is that it is
    // never held across a call that can block on the radio.
    int8_t rssi = 0;
    bool have_rssi = false;
    if (started_ && status_.link == Link::kConnected) {
        wifi_ap_record_t record = {};
        if (esp_wifi_sta_get_ap_info(&record) == ESP_OK) {
            rssi = record.rssi;
            have_rssi = true;
        }
    }

    xSemaphoreTake(lock_, portMAX_DELAY);
    Status copy = status_;
    xSemaphoreGive(lock_);
    if (have_rssi) {
        copy.rssi = rssi;
    }
    return copy;
}

// **The scan covers 2.4 GHz because that is every band this chip has**, and
// the assertion below is the only honest way to write that down: it is not a
// choice this code made and not one it can revisit. ESP-IDF marks a dual-band
// part with `SOC_WIFI_SUPPORT_5G` — defined for the ESP32-C5, absent for the
// C6 on this board (`soc/esp32c6/include/soc/soc_caps.h`, checked, not
// assumed). There is no `WIFI_BAND_MODE_AUTO` to set here and no 5 GHz channel
// bitmap to fill in; the radio does not receive on those frequencies.
//
// So this line exists for the day somebody ports this to a part that does:
// then `esp_wifi_set_band_mode(WIFI_BAND_MODE_AUTO)` belongs in `Init`, the
// scan should report each result's band, and §10.9's "2.4 GHz only" paragraph
// stops being true. A compile error is the right way to be told that, and it
// costs nothing until it happens.
#if defined(SOC_WIFI_SUPPORT_5G) && SOC_WIFI_SUPPORT_5G
#error "This chip has a 5 GHz radio: Scan() must select the band and report it (§10.9)."
#endif

esp_err_t Radio::Scan(ScanResult *out, size_t capacity, size_t *found) {
    if (out == nullptr || found == nullptr || capacity == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    *found = 0;
    if (!ready_) {
        return ESP_ERR_INVALID_STATE;
    }
    if (scanning_) {
        return ESP_ERR_INVALID_STATE;
    }

    // **Set before anything is brought up**, so that nothing can start an
    // association in the window between the station going up and the scan
    // beginning: `StartClient` and `StartAp` both refuse while this is true.
    scanning_ = true;

    // **A scan with the radio off brings the station up on its own and puts it
    // back.** "What is on the air" is the first question an operator asks and
    // the last one that should require the device to already be somewhere.
    //
    // The station interface, and nothing else. Raising an access point to make
    // a scan possible — even a hidden one — is strictly *more* radio than the
    // job needs: a hidden AP still beacons (hidden means the SSID field is
    // blank, not that the AP is silent), still holds a channel and still
    // starts a DHCP server, and all of it would have to be undone afterwards.
    // A started station transmits probe requests while scanning and nothing at
    // all otherwise.
    //
    // **Whatever had to be raised is put back**, at both levels: the Wi-Fi
    // stack costs about 41 KB of heap (160 KB free with it down against
    // 119 KB with it up, measured on this board), so a scan on a device that
    // lives with the radio off borrows that for two seconds rather than
    // keeping it until the next reboot.
    const bool stack_was_ours = !stack_ready_;
    const bool station_was_off = !started_;
    if (stack_was_ours) {
        const esp_err_t up = EnsureStack();
        if (up != ESP_OK) {
            scanning_ = false;
            ESP_LOGW(TAG, "scan needed the stack up and it would not start: %s",
                     esp_err_to_name(up));
            return up;
        }
    }
    if (station_was_off) {
        esp_err_t up = esp_wifi_set_mode(WIFI_MODE_STA);
        if (up == ESP_OK) {
            up = EnsureStarted();
        }
        if (up != ESP_OK) {
            if (stack_was_ours) {
                ReleaseStack();
            }
            scanning_ = false;
            ESP_LOGW(TAG, "scan needed the station up and it would not start: %s",
                     esp_err_to_name(up));
            return up;
        }
    }

    // Undoes exactly what was raised, in one place, so the three exits below
    // cannot disagree about it.
    const auto put_back = [this, stack_was_ours, station_was_off]() {
        if (stack_was_ours) {
            ReleaseStack();
        } else if (station_was_off) {
            StopStation();
        }
    };

    wifi_scan_config_t config = {};  // active scan, every channel, no filter
    esp_err_t err = esp_wifi_scan_start(&config, true);
    if (err != ESP_OK) {
        put_back();
        scanning_ = false;
        ESP_LOGW(TAG, "scan: %s", esp_err_to_name(err));
        return err;
    }

    uint16_t number = kMaxScanResults;
    err = esp_wifi_scan_get_ap_records(&number, scan_records);
    // Back to how it was found, and *before* `scanning_` is cleared, so the
    // manager cannot start something in between and have it torn down.
    put_back();
    scanning_ = false;
    if (err != ESP_OK) {
        // The records are freed by the call above whether or not it succeeded;
        // this is the belt-and-braces for the path where it did not run at all.
        esp_wifi_clear_ap_list();
        return err;
    }

    // Already sorted by signal — `esp_wifi_scan_get_ap_records` returns them
    // strongest first, which is the order §10.8.6's list wants.
    const size_t limit = number < capacity ? number : capacity;
    for (size_t i = 0; i < limit; ++i) {
        CopyField(out[i].ssid, sizeof(out[i].ssid),
                  reinterpret_cast<const char *>(scan_records[i].ssid));
        out[i].rssi = scan_records[i].rssi;
        out[i].channel = scan_records[i].primary;
        out[i].secured = scan_records[i].authmode != WIFI_AUTH_OPEN;
    }
    *found = limit;
    return ESP_OK;
}

// --- events -----------------------------------------------------------------

void Radio::EventTrampoline(void *arg, const char *base, int32_t id, void *data) {
    static_cast<Radio *>(arg)->OnEvent(base, id, data);
}

void Radio::SetLink(Link link, Failure failure, uint8_t reason) {
    xSemaphoreTake(lock_, portMAX_DELAY);
    status_.link = link;
    status_.failure = failure;
    status_.reason = reason;
    if (link != Link::kConnected) {
        status_.ip = 0;
    }
    ++status_.changes;
    xSemaphoreGive(lock_);
}

void Radio::OnEvent(const char *base, int32_t id, void *data) {
    if (base == WIFI_EVENT) {
        switch (id) {
            case WIFI_EVENT_STA_START:
                if (want_connect_) {
                    want_connect_ = false;
                    const esp_err_t err = esp_wifi_connect();
                    if (err != ESP_OK) {
                        ESP_LOGW(TAG, "connect on start: %s", esp_err_to_name(err));
                        SetLink(Link::kFailed, Failure::kOther, 0);
                    }
                }
                break;

            case WIFI_EVENT_STA_CONNECTED:
                // Associated, but with no address yet. This is the moment the
                // fixed one goes on — the same point the house firmware of
                // §10.14.4 picks, and for the same reason: the interface is up
                // and the DHCP client has not had time to get anywhere.
                ApplyAddressing();
                break;

            case WIFI_EVENT_STA_DISCONNECTED: {
                if (suppress_disconnect_ > 0) {
                    // Ours. We tore the association down to start another one,
                    // and the new attempt is already recorded as connecting.
                    suppress_disconnect_ = static_cast<uint8_t>(suppress_disconnect_ - 1);
                    break;
                }
                const auto *event = static_cast<const wifi_event_sta_disconnected_t *>(data);
                const uint8_t reason = event != nullptr ? event->reason : 0;
                ESP_LOGW(TAG, "disconnected: reason %u (%s)", static_cast<unsigned>(reason),
                         Name(Classify(reason)));
                SetLink(Link::kFailed, Classify(reason), reason);
                break;
            }

            case WIFI_EVENT_AP_START: {
                esp_netif_ip_info_t info = {};
                if (ap_netif_ != nullptr &&
                    esp_netif_get_ip_info(static_cast<esp_netif_t *>(ap_netif_), &info) == ESP_OK) {
                    xSemaphoreTake(lock_, portMAX_DELAY);
                    status_.ip = info.ip.addr;
                    xSemaphoreGive(lock_);
                }
                break;
            }

            case WIFI_EVENT_AP_STACONNECTED:
                xSemaphoreTake(lock_, portMAX_DELAY);
                if (status_.clients < 0xFF) {
                    ++status_.clients;
                }
                ++status_.changes;
                xSemaphoreGive(lock_);
                break;

            case WIFI_EVENT_AP_STADISCONNECTED:
                xSemaphoreTake(lock_, portMAX_DELAY);
                if (status_.clients > 0) {
                    --status_.clients;
                }
                ++status_.changes;
                xSemaphoreGive(lock_);
                break;

            default:
                break;
        }
        return;
    }

    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const auto *event = static_cast<const ip_event_got_ip_t *>(data);
        esp_ip4_addr_t address = {};
        if (event != nullptr) {
            address = event->ip_info.ip;
        }
        xSemaphoreTake(lock_, portMAX_DELAY);
        status_.link = Link::kConnected;
        status_.failure = Failure::kNone;
        status_.reason = 0;
        status_.ip = address.addr;
        status_.ip_is_static = static_ip_.enabled;
        ++status_.changes;
        xSemaphoreGive(lock_);
        ESP_LOGI(TAG, "connected to '%s', ip " IPSTR " (%s)", status_.ssid, IP2STR(&address),
                 static_ip_.enabled ? "static" : "dhcp");
    }
}

}  // namespace wifi
