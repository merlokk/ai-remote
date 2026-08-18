#include "config.h"

#include <cerrno>
#include <cstdio>
#include <cstring>

#include "cJSON.h"
#include "esp_log.h"
#include "storage.h"
#include "timezone.h"

namespace config {

namespace {

constexpr const char *TAG = "config";

Data data = {};
bool loaded = false;

// What `KEY` decided at boot (§10.15). Kept because it has to be said twice —
// once on the screen, once whenever the console is asked — and neither of those
// exists yet at the moment the button is read.
RestoreOutcome boot_restore = RestoreOutcome::kNotRequested;

// One buffer, used for reading and for printing. Playback has its own; these
// two never run at the same time, and neither allocates (§10.14.1).
char file_buffer[kMaxFileSize];

// Copies a JSON string into a fixed field, refusing rather than truncating: a
// silently shortened SSID or password fails to connect and gives no hint why.
void CopyString(const cJSON *object, const char *name, char *out, size_t capacity) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    if (!cJSON_IsString(item) || item->valuestring == nullptr) {
        return;
    }
    const size_t length = strlen(item->valuestring);
    if (length >= capacity) {
        ESP_LOGW(TAG, "%s is %u bytes, the field holds %u — left at its previous value", name,
                 static_cast<unsigned>(length), static_cast<unsigned>(capacity - 1));
        return;
    }
    memcpy(out, item->valuestring, length + 1);
}

void CopyBool(const cJSON *object, const char *name, bool *out) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    if (cJSON_IsBool(item)) {
        *out = cJSON_IsTrue(item) != 0;
    }
}

// Numbers arrive as doubles and are clamped here rather than where they are
// used. A brightness of 130 from a hand-edited file must not reach the panel,
// and a caller that has to re-check every field is a caller that will forget.
void CopyNumber(const cJSON *object, const char *name, long min, long max, long *out) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    if (!cJSON_IsNumber(item)) {
        return;
    }
    long value = static_cast<long>(item->valuedouble);
    if (value < min) {
        value = min;
    } else if (value > max) {
        value = max;
    }
    *out = value;
}

// Reads one field into a fixed buffer and keeps it only if it is an address.
// A typo is dropped rather than passed to lwIP, and the drop is logged with
// the field's name — an operator who wrote `10.0.0.300` needs to be told
// which of the five strings was the problem.
bool CopyAddress(const cJSON *object, const char *name, char *out, size_t capacity) {
    out[0] = '\0';
    CopyString(object, name, out, capacity);
    if (out[0] == '\0') {
        return false;
    }
    uint32_t ignored = 0;
    if (!ParseIpv4(out, &ignored)) {
        ESP_LOGW(TAG, "ip.%s is '%s', which is not an address; dropped", name, out);
        out[0] = '\0';
        return false;
    }
    return true;
}

esp_err_t ReadFileInto(const char *path, size_t *length) {
    const esp_err_t err = storage::ReadFile(path, file_buffer, sizeof(file_buffer), length);
    if (err == ESP_ERR_INVALID_SIZE) {
        ESP_LOGW(TAG, "%s is %u bytes, the cap is %u", path, static_cast<unsigned>(*length),
                 static_cast<unsigned>(sizeof(file_buffer) - 1));
    }
    return err;
}

esp_err_t Parse(const char *json, Data *out) {
    cJSON *root = cJSON_Parse(json);
    if (root == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    // **Valid JSON is not the same as a config.** `[]`, `42` and `"hello"` all
    // parse; every `GetObjectItem` below then answers null and the file reads
    // as "an object with no fields in it", so a device would come up on the
    // defaults *and leave the rubbish on the filesystem to do it again next
    // boot*. §10.15 says an unusable file is restored — this is the shape of
    // unusable that still parses.
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    // Defaults first, then whatever the file says on top: every field may be
    // missing, and a half-written file leaves the rest at sane values rather
    // than at zero (§10.15's "every field missing" test).
    FillDefaults(out);

    const cJSON *wifi = cJSON_GetObjectItemCaseSensitive(root, "wifi");
    if (cJSON_IsObject(wifi)) {
        CopyBool(wifi, "active", &out->wifi.active);

        // **A mode that is neither is left at the default rather than
        // guessed.** The two spellings are the whole vocabulary, and reading
        // "AP " or "station" as one of them would be the same class of error
        // as libc reading a misspelled zone as UTC (§10.8.2).
        const cJSON *mode = cJSON_GetObjectItemCaseSensitive(wifi, "mode");
        if (cJSON_IsString(mode) && mode->valuestring != nullptr) {
            if (strcmp(mode->valuestring, "ap") == 0) {
                out->wifi.mode = WifiMode::kAp;
            } else if (strcmp(mode->valuestring, "client") == 0) {
                out->wifi.mode = WifiMode::kClient;
            } else {
                ESP_LOGW(TAG, "wifi.mode '%s' is neither 'client' nor 'ap'; left at '%s'",
                         mode->valuestring,
                         out->wifi.mode == WifiMode::kAp ? "ap" : "client");
            }
        }

        long value = out->wifi.rounds_before_ap;
        CopyNumber(wifi, "rounds", 0, 255, &value);
        out->wifi.rounds_before_ap = static_cast<uint8_t>(value);

        value = out->wifi.ap_window_seconds;
        CopyNumber(wifi, "apWindowSeconds", 0, 65535, &value);
        out->wifi.ap_window_seconds = static_cast<uint16_t>(value);

        const cJSON *ap = cJSON_GetObjectItemCaseSensitive(wifi, "ap");
        if (cJSON_IsObject(ap)) {
            CopyString(ap, "ssid", out->wifi.ap_ssid, sizeof(out->wifi.ap_ssid));
            CopyString(ap, "password", out->wifi.ap_password, sizeof(out->wifi.ap_password));
            // 2.4 GHz, and the ESP32-C6 has no other band (§10.9).
            value = out->wifi.ap_channel;
            CopyNumber(ap, "channel", 1, 13, &value);
            out->wifi.ap_channel = static_cast<uint8_t>(value);
        }

        const cJSON *networks = cJSON_GetObjectItemCaseSensitive(wifi, "networks");
        if (cJSON_IsArray(networks)) {
            out->wifi.network_count = 0;
            const cJSON *entry = nullptr;
            cJSON_ArrayForEach(entry, networks) {
                if (out->wifi.network_count >= kMaxNetworks) {
                    ESP_LOGW(TAG, "more than %u networks in the file; the rest are ignored",
                             static_cast<unsigned>(kMaxNetworks));
                    break;
                }
                if (!cJSON_IsObject(entry)) {
                    continue;
                }
                Network &slot = out->wifi.networks[out->wifi.network_count];
                slot = Network{};
                CopyString(entry, "ssid", slot.ssid, sizeof(slot.ssid));
                CopyString(entry, "password", slot.password, sizeof(slot.password));

                // The fixed address for *this* network (§10.9), and DHCP when
                // there is none. Per network rather than per device: a desk
                // object that moves between a home with DHCP and an office
                // that hands out nothing needs one of each.
                const cJSON *ip = cJSON_GetObjectItemCaseSensitive(entry, "ip");
                if (cJSON_IsObject(ip)) {
                    CopyBool(ip, "static", &slot.ip.enabled);
                    const bool address = CopyAddress(ip, "address", slot.ip.address,
                                                     sizeof(slot.ip.address));
                    const bool netmask = CopyAddress(ip, "netmask", slot.ip.netmask,
                                                     sizeof(slot.ip.netmask));
                    const bool gateway = CopyAddress(ip, "gateway", slot.ip.gateway,
                                                     sizeof(slot.ip.gateway));
                    // Optional, and a typo in one costs name resolution rather
                    // than the interface.
                    CopyAddress(ip, "dns1", slot.ip.dns1, sizeof(slot.ip.dns1));
                    CopyAddress(ip, "dns2", slot.ip.dns2, sizeof(slot.ip.dns2));

                    // **Three fields or none.** An interface with an address
                    // and no route is a device that looks connected and can
                    // reach nothing, which is worse than one that fell back to
                    // DHCP and said so. The SSID survives either way — losing
                    // a working network over a typo in an optional field would
                    // be the wrong trade.
                    if (slot.ip.enabled && !(address && netmask && gateway)) {
                        ESP_LOGW(TAG,
                                 "'%s' asks for a static address but %s is missing or bad; "
                                 "falling back to DHCP",
                                 slot.ssid, !address ? "address" : (!netmask ? "netmask" : "gateway"));
                        slot.ip.enabled = false;
                    }
                }

                if (slot.ssid[0] != '\0') {
                    ++out->wifi.network_count;
                }
            }
        }
    }

    const cJSON *internet = cJSON_GetObjectItemCaseSensitive(root, "internet");
    if (cJSON_IsObject(internet)) {
        CopyBool(internet, "check", &out->internet.check);

        long value = out->internet.interval_seconds;
        // A floor of five seconds rather than zero: this is somebody else's
        // server being asked, and a probe loop with no interval is a flood
        // with a friendly name.
        CopyNumber(internet, "intervalSeconds", 5, 65535, &value);
        out->internet.interval_seconds = static_cast<uint16_t>(value);

        value = out->internet.timeout_ms;
        CopyNumber(internet, "timeoutMs", 100, 65535, &value);
        out->internet.timeout_ms = static_cast<uint16_t>(value);

        value = out->internet.failures_before_offline;
        CopyNumber(internet, "failures", 1, 255, &value);
        out->internet.failures_before_offline = static_cast<uint8_t>(value);

        const cJSON *targets = cJSON_GetObjectItemCaseSensitive(internet, "targets");
        if (cJSON_IsArray(targets)) {
            // The file's list replaces the built-in one entirely, the way
            // `networks` does — an empty array means "none", not "the
            // defaults", because the operator wrote it down.
            out->internet.target_count = 0;
            const cJSON *entry = nullptr;
            cJSON_ArrayForEach(entry, targets) {
                if (out->internet.target_count >= kMaxProbeTargets) {
                    ESP_LOGW(TAG, "more than %u internet targets; the rest are ignored",
                             static_cast<unsigned>(kMaxProbeTargets));
                    break;
                }
                if (!cJSON_IsString(entry) || entry->valuestring == nullptr) {
                    continue;
                }
                uint32_t parsed = 0;
                if (!ParseIpv4(entry->valuestring, &parsed)) {
                    // **A name is refused as firmly as a typo**, and that is
                    // deliberate: this is an ICMP echo to an address, there is
                    // no resolver in the path, and "internet.targets" holding
                    // `google.com` would be a check that can never pass.
                    ESP_LOGW(TAG, "internet target '%s' is not an IPv4 address; dropped",
                             entry->valuestring);
                    continue;
                }
                snprintf(out->internet.targets[out->internet.target_count],
                         kIpTextSize, "%s", entry->valuestring);
                ++out->internet.target_count;
            }
            if (out->internet.check && out->internet.target_count == 0) {
                ESP_LOGW(TAG, "internet.check is on with nothing to ping; it will stay unknown");
            }
        }
    }

    const cJSON *nats = cJSON_GetObjectItemCaseSensitive(root, "nats");
    if (cJSON_IsObject(nats)) {
        CopyString(nats, "url", out->nats.url, sizeof(out->nats.url));
    }

    const cJSON *time = cJSON_GetObjectItemCaseSensitive(root, "time");
    if (cJSON_IsObject(time)) {
        CopyString(time, "zone", out->time.zone, sizeof(out->time.zone));
        CopyString(time, "posix", out->time.posix, sizeof(out->time.posix));
        CopyString(time, "sntp", out->time.sntp_server, sizeof(out->time.sntp_server));

        // Hours, and **0 is off** rather than a floor of one: "never sync" has
        // to be expressible, and a field that silently became hourly would be
        // a device asking a stranger's server 24 times a day because somebody
        // typed a zero.
        long value = out->time.sync_hours;
        CopyNumber(time, "syncHours", 0, 255, &value);
        out->time.sync_hours = static_cast<uint8_t>(value);

        // A file that names a zone but carries no rule — hand-edited, or
        // written by a firmware whose table was smaller — is completed from
        // the table rather than refused. The name is the operator's intent;
        // the rule is an implementation detail they should not have to know.
        const char *known = tz::Lookup(out->time.zone);
        if (known != nullptr &&
            (out->time.posix[0] == '\0' ||
             cJSON_GetObjectItemCaseSensitive(time, "posix") == nullptr)) {
            snprintf(out->time.posix, sizeof(out->time.posix), "%s", known);
        }
    }

    const cJSON *display = cJSON_GetObjectItemCaseSensitive(root, "display");
    if (cJSON_IsObject(display)) {
        long value = out->display.brightness;
        CopyNumber(display, "brightness", 0, 100, &value);
        out->display.brightness = static_cast<uint8_t>(value);

        value = out->display.dim_seconds;
        CopyNumber(display, "dimSeconds", 0, 65535, &value);
        out->display.dim_seconds = static_cast<uint16_t>(value);

        value = out->display.blank_seconds;
        CopyNumber(display, "blankSeconds", 0, 65535, &value);
        out->display.blank_seconds = static_cast<uint16_t>(value);
    }

    const cJSON *audio = cJSON_GetObjectItemCaseSensitive(root, "audio");
    if (cJSON_IsObject(audio)) {
        long value = out->audio.volume_percent;
        CopyNumber(audio, "volume", 0, 100, &value);
        out->audio.volume_percent = static_cast<uint8_t>(value);
    }

    // The touch correction (§10.8.5). **Clamped here as well as refused at the
    // fit**, because this file can be edited by hand: a scale of 30000 typed
    // into it would be a screen nobody can press, and the calibration screen
    // that fixes it is reached by pressing something.
    const cJSON *touch = cJSON_GetObjectItemCaseSensitive(root, "touch");
    if (cJSON_IsObject(touch)) {
        long value = out->touch.scale_x;
        CopyNumber(touch, "scaleX", -2000, 2000, &value);
        out->touch.scale_x = static_cast<int16_t>(value);

        value = out->touch.scale_y;
        CopyNumber(touch, "scaleY", -2000, 2000, &value);
        out->touch.scale_y = static_cast<int16_t>(value);

        value = out->touch.offset_x;
        CopyNumber(touch, "offsetX", -480, 480, &value);
        out->touch.offset_x = static_cast<int16_t>(value);

        value = out->touch.offset_y;
        CopyNumber(touch, "offsetY", -480, 480, &value);
        out->touch.offset_y = static_cast<int16_t>(value);
    }

    cJSON_Delete(root);
    return ESP_OK;
}

// Serialises into `file_buffer`. cJSON allocates while building the tree —
// that is a library's heap use in a one-shot path, which §10.14.1 allows; the
// *output* goes into our own buffer, so a config that outgrows the cap fails
// here rather than in the filesystem.
esp_err_t Serialise(const Data &in, size_t *length) {
    cJSON *root = cJSON_CreateObject();
    if (root == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    cJSON *wifi = cJSON_AddObjectToObject(root, "wifi");
    cJSON *networks = nullptr;
    if (wifi != nullptr) {
        cJSON_AddBoolToObject(wifi, "active", in.wifi.active);
        cJSON_AddStringToObject(wifi, "mode", in.wifi.mode == WifiMode::kAp ? "ap" : "client");
        cJSON_AddNumberToObject(wifi, "rounds", in.wifi.rounds_before_ap);
        cJSON_AddNumberToObject(wifi, "apWindowSeconds", in.wifi.ap_window_seconds);
        cJSON *ap = cJSON_AddObjectToObject(wifi, "ap");
        if (ap != nullptr) {
            cJSON_AddStringToObject(ap, "ssid", in.wifi.ap_ssid);
            cJSON_AddStringToObject(ap, "password", in.wifi.ap_password);
            cJSON_AddNumberToObject(ap, "channel", in.wifi.ap_channel);
        }
        networks = cJSON_AddArrayToObject(wifi, "networks");
    }
    if (networks != nullptr) {
        for (uint8_t i = 0; i < in.wifi.network_count && i < kMaxNetworks; ++i) {
            cJSON *entry = cJSON_CreateObject();
            if (entry == nullptr) {
                break;
            }
            const Network &network = in.wifi.networks[i];
            cJSON_AddStringToObject(entry, "ssid", network.ssid);
            cJSON_AddStringToObject(entry, "password", network.password);

            // Written only when there is something to write. An empty `ip`
            // object on every entry would be noise in a file people edit by
            // hand — and anything typed is preserved even while `static` is
            // off, so turning it back on does not mean typing it again.
            const StaticIp &ip = network.ip;
            if (ip.enabled || ip.address[0] != '\0' || ip.netmask[0] != '\0' ||
                ip.gateway[0] != '\0' || ip.dns1[0] != '\0' || ip.dns2[0] != '\0') {
                cJSON *ip_object = cJSON_AddObjectToObject(entry, "ip");
                if (ip_object != nullptr) {
                    cJSON_AddBoolToObject(ip_object, "static", ip.enabled);
                    cJSON_AddStringToObject(ip_object, "address", ip.address);
                    cJSON_AddStringToObject(ip_object, "netmask", ip.netmask);
                    cJSON_AddStringToObject(ip_object, "gateway", ip.gateway);
                    cJSON_AddStringToObject(ip_object, "dns1", ip.dns1);
                    cJSON_AddStringToObject(ip_object, "dns2", ip.dns2);
                }
            }
            cJSON_AddItemToArray(networks, entry);
        }
    }

    cJSON *internet = cJSON_AddObjectToObject(root, "internet");
    if (internet != nullptr) {
        cJSON_AddBoolToObject(internet, "check", in.internet.check);
        cJSON_AddNumberToObject(internet, "intervalSeconds", in.internet.interval_seconds);
        cJSON_AddNumberToObject(internet, "timeoutMs", in.internet.timeout_ms);
        cJSON_AddNumberToObject(internet, "failures", in.internet.failures_before_offline);
        cJSON *targets = cJSON_AddArrayToObject(internet, "targets");
        if (targets != nullptr) {
            for (uint8_t i = 0; i < in.internet.target_count && i < kMaxProbeTargets; ++i) {
                cJSON *item = cJSON_CreateString(in.internet.targets[i]);
                if (item == nullptr) {
                    break;
                }
                cJSON_AddItemToArray(targets, item);
            }
        }
    }

    cJSON *nats = cJSON_AddObjectToObject(root, "nats");
    if (nats != nullptr) {
        cJSON_AddStringToObject(nats, "url", in.nats.url);
    }

    cJSON *time = cJSON_AddObjectToObject(root, "time");
    if (time != nullptr) {
        cJSON_AddStringToObject(time, "zone", in.time.zone);
        cJSON_AddStringToObject(time, "posix", in.time.posix);
        cJSON_AddStringToObject(time, "sntp", in.time.sntp_server);
        cJSON_AddNumberToObject(time, "syncHours", in.time.sync_hours);
    }

    cJSON *display = cJSON_AddObjectToObject(root, "display");
    if (display != nullptr) {
        cJSON_AddNumberToObject(display, "brightness", in.display.brightness);
        cJSON_AddNumberToObject(display, "dimSeconds", in.display.dim_seconds);
        cJSON_AddNumberToObject(display, "blankSeconds", in.display.blank_seconds);
    }

    cJSON *audio = cJSON_AddObjectToObject(root, "audio");
    if (audio != nullptr) {
        cJSON_AddNumberToObject(audio, "volume", in.audio.volume_percent);
    }

    cJSON *touch = cJSON_AddObjectToObject(root, "touch");
    if (touch != nullptr) {
        cJSON_AddNumberToObject(touch, "scaleX", in.touch.scale_x);
        cJSON_AddNumberToObject(touch, "scaleY", in.touch.scale_y);
        cJSON_AddNumberToObject(touch, "offsetX", in.touch.offset_x);
        cJSON_AddNumberToObject(touch, "offsetY", in.touch.offset_y);
    }

    const cJSON_bool ok =
        cJSON_PrintPreallocated(root, file_buffer, static_cast<int>(sizeof(file_buffer)), 1);
    cJSON_Delete(root);
    if (!ok) {
        return ESP_ERR_INVALID_SIZE;
    }
    *length = strlen(file_buffer);
    return ESP_OK;
}

esp_err_t WriteAtomically(const char *contents, size_t length) {
    // **The dance itself moved to `storage`** (§10.15): SPIFFS will not rename
    // onto an existing name, so the write is temp → remove → rename, and the
    // window that opens is closed at the next boot. There are two files that need
    // that now — this one and the registration of §10.7 — and a second copy of it
    // would be the copy that drifts.
    return storage::WriteFileAtomically(kPath, kTempPath, contents, length);
}

}  // namespace

bool ParseIpv4(const char *text, uint32_t *out) {
    if (text == nullptr || out == nullptr) {
        return false;
    }

    uint32_t value = 0;
    const char *p = text;
    for (int octet = 0; octet < 4; ++octet) {
        if (octet > 0) {
            if (*p != '.') {
                return false;
            }
            ++p;
        }
        if (*p < '0' || *p > '9') {
            // Catches the empty string, `192.168..1`, a leading space, and a
            // `0x7f` that `inet_aton` would happily read as hexadecimal.
            return false;
        }
        // **A leading zero is refused, not interpreted.** `010` is ten to the
        // person who typed it and eight to `inet_aton`, and a device quietly
        // on 8.1.1.1 instead of 10.1.1.1 is a long evening.
        const bool leading_zero = *p == '0' && p[1] >= '0' && p[1] <= '9';
        uint32_t part = 0;
        int digits = 0;
        while (*p >= '0' && *p <= '9') {
            part = part * 10 + static_cast<uint32_t>(*p - '0');
            ++p;
            if (++digits > 3 || part > 255) {
                return false;
            }
        }
        if (leading_zero) {
            return false;
        }
        // First octet in the low byte — what lwIP calls network order, and
        // what the console's own `%u.%u.%u.%u` already assumes.
        value |= part << (8 * octet);
    }
    if (*p != '\0') {
        return false;  // trailing anything: "1.2.3.4.5", "1.2.3.4 ", a newline
    }

    *out = value;
    return true;
}

void FillDefaults(Data *out) {
    if (out == nullptr) {
        return;
    }
    *out = Data{};
    out->wifi.active = false;
    out->wifi.mode = WifiMode::kClient;
    out->wifi.network_count = 0;
    out->wifi.rounds_before_ap = 2;
    out->wifi.ap_window_seconds = 120;
    snprintf(out->wifi.ap_ssid, sizeof(out->wifi.ap_ssid), "approver-esp32");
    out->wifi.ap_password[0] = '\0';
    out->wifi.ap_channel = 6;
    // §10.9's minute, and three operators rather than one — Google, Cloudflare
    // and Quad9. Three because a single blocked host must not read as an
    // outage, and *those* three because they are the anycast resolvers most
    // likely to answer ICMP from anywhere this device is plugged in.
    out->internet.check = true;
    out->internet.interval_seconds = 60;
    out->internet.timeout_ms = 2000;
    out->internet.failures_before_offline = 2;
    snprintf(out->internet.targets[0], kIpTextSize, "8.8.8.8");
    snprintf(out->internet.targets[1], kIpTextSize, "1.1.1.1");
    snprintf(out->internet.targets[2], kIpTextSize, "9.9.9.9");
    out->internet.target_count = 3;

    // The bus this device is actually built against (§10.3): the NATS server
    // on the home LAN, no TLS, no credentials. Unlike `sntp` below it names a
    // machine of the operator's own rather than a stranger's, so having a
    // default here is the difference between a restored device that connects
    // and one that has to be told where the bus is over USB first.
    snprintf(out->nats.url, sizeof(out->nats.url), "nats://192.168.11.70:4222");
    snprintf(out->time.zone, sizeof(out->time.zone), "UTC");
    snprintf(out->time.posix, sizeof(out->time.posix), "UTC0");
    // **Empty on purpose, and the only string field that is.** Every other
    // default here is a number this firmware can pick for itself; this one
    // names somebody else's machine, and a device that talks to a host the
    // operator never wrote down is the same mistake `internet.targets` refuses
    // to make. No server in the file means no server — and therefore no clock
    // sync, rather than an exchange that fails every interval forever. The
    // shipped `config.init.json` names `pool.ntp.org`, so a device that can
    // read its filesystem does sync; this is what a device that cannot falls
    // back to.
    out->time.sntp_server[0] = '\0';
    out->time.sync_hours = 6;
    out->display.brightness = 80;
    out->display.dim_seconds = 30;
    out->display.blank_seconds = 120;
    out->audio.volume_percent = 80;

    // No correction. A device that has never been calibrated reports the
    // controller's own coordinates, which on this panel are the screen's — the
    // CST9220's grid is 480x480 and so is the glass.
    out->touch.scale_x = 1000;
    out->touch.scale_y = 1000;
    out->touch.offset_x = 0;
    out->touch.offset_y = 0;
}

bool Loaded() { return loaded; }

Data &Get() { return data; }

esp_err_t Reload() {
    if (!storage::Mounted()) {
        return ESP_ERR_INVALID_STATE;
    }

    size_t length = 0;
    esp_err_t err = ReadFileInto(kPath, &length);
    if (err != ESP_OK) {
        return err;
    }

    err = Parse(file_buffer, &data);
    if (err != ESP_OK) {
        return err;
    }
    loaded = true;
    return ESP_OK;
}

esp_err_t Restore() {
    if (!storage::Mounted()) {
        return ESP_ERR_INVALID_STATE;
    }

    size_t length = 0;
    esp_err_t err = ReadFileInto(kDefaultsPath, &length);
    if (err != ESP_OK) {
        // §10.15: a missing `config.init.json` is a **build** error — it ships
        // in the image — not a runtime state to design around. Say so loudly
        // and fall back to the compiled-in values rather than to zeros.
        ESP_LOGE(TAG, "%s unreadable (%s); using the built-in defaults", kDefaultsPath,
                 esp_err_to_name(err));
        FillDefaults(&data);
        loaded = true;
        return err;
    }

    // Written through the same atomic path as any other save, so an interrupted
    // restore cannot leave a truncated config behind — which would break the
    // exact recovery being performed.
    err = WriteAtomically(file_buffer, length);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "could not write %s: %s", kPath, esp_err_to_name(err));
    }

    err = Parse(file_buffer, &data);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s does not parse either; using the built-in defaults", kDefaultsPath);
        FillDefaults(&data);
    }
    loaded = true;
    return err;
}

RestoreOutcome RestoreAtBoot(bool key_held) {
    boot_restore = RestoreOutcome::kNotRequested;
    if (!key_held) {
        return boot_restore;
    }

    // One log line at the time, because there is no panel this early (§10.15) —
    // the screen says it again as soon as there is one.
    ESP_LOGW(TAG, "KEY held at boot: putting %s back over %s", kDefaultsPath, kPath);

    const esp_err_t err = Restore();
    if (err == ESP_OK) {
        boot_restore = RestoreOutcome::kRestored;
        ESP_LOGW(TAG, "%s restored; the registration was not touched", kPath);
    } else {
        boot_restore = RestoreOutcome::kFailed;
        ESP_LOGE(TAG, "%s not restored (%s); the settings are as they were", kPath,
                 esp_err_to_name(err));
    }
    return boot_restore;
}

RestoreOutcome BootRestore() { return boot_restore; }

const char *BootRestoreText() {
    switch (boot_restore) {
        case RestoreOutcome::kRestored:
            return "config restored";
        case RestoreOutcome::kFailed:
            return "config restore failed";
        case RestoreOutcome::kNotRequested:
            break;
    }
    return nullptr;
}

esp_err_t Save() {
    if (!storage::Mounted()) {
        return ESP_ERR_INVALID_STATE;
    }

    size_t length = 0;
    const esp_err_t err = Serialise(data, &length);
    if (err != ESP_OK) {
        return err;
    }
    return WriteAtomically(file_buffer, length);
}

esp_err_t Init() {
    FillDefaults(&data);

    if (!storage::Mounted()) {
        ESP_LOGE(TAG, "storage is not mounted; running on the built-in defaults");
        return ESP_ERR_INVALID_STATE;
    }

    // Before anything is read: finish a write that a power cut interrupted.
    storage::RecoverInterruptedWrite(kPath, kTempPath);

    const esp_err_t err = Reload();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "%s loaded: %u network(s), volume %u%%", kPath,
                 static_cast<unsigned>(data.wifi.network_count),
                 static_cast<unsigned>(data.audio.volume_percent));
        return ESP_OK;
    }

    // Missing, oversized or unparseable — all one outcome (§10.15): put the
    // defaults back, one log line, and carry on booting.
    ESP_LOGW(TAG, "%s not usable (%s); restoring from %s", kPath, esp_err_to_name(err),
             kDefaultsPath);
    return Restore();
}

}  // namespace config
