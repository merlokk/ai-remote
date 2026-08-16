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
                slot.ssid[0] = '\0';
                slot.password[0] = '\0';
                CopyString(entry, "ssid", slot.ssid, sizeof(slot.ssid));
                CopyString(entry, "password", slot.password, sizeof(slot.password));
                if (slot.ssid[0] != '\0') {
                    ++out->wifi.network_count;
                }
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
        networks = cJSON_AddArrayToObject(wifi, "networks");
    }
    if (networks != nullptr) {
        for (uint8_t i = 0; i < in.wifi.network_count && i < kMaxNetworks; ++i) {
            cJSON *entry = cJSON_CreateObject();
            if (entry == nullptr) {
                break;
            }
            cJSON_AddStringToObject(entry, "ssid", in.wifi.networks[i].ssid);
            cJSON_AddStringToObject(entry, "password", in.wifi.networks[i].password);
            cJSON_AddItemToArray(networks, entry);
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
    char temp_path[storage::kMaxPathLength];
    char final_path[storage::kMaxPathLength];
    if (!storage::ResolvePath(kTempPath, temp_path, sizeof(temp_path)) ||
        !storage::ResolvePath(kPath, final_path, sizeof(final_path))) {
        return ESP_ERR_INVALID_ARG;
    }

    FILE *file = fopen(temp_path, "wb");
    if (file == nullptr) {
        ESP_LOGE(TAG, "open %s failed: errno %d", temp_path, errno);
        return ESP_FAIL;
    }
    const size_t written = fwrite(contents, 1, length, file);
    // Both of these can fail on a full filesystem, and a close that failed is a
    // file that may not be on the medium — so the rename is not attempted.
    const bool flushed = fflush(file) == 0;
    const bool closed = fclose(file) == 0;
    if (written != length || !flushed || !closed) {
        ESP_LOGE(TAG, "write %s failed: %u of %u bytes, flush %d, close %d, errno %d", temp_path,
                 static_cast<unsigned>(written), static_cast<unsigned>(length), flushed, closed,
                 errno);
        remove(temp_path);
        return ESP_FAIL;
    }

    // **SPIFFS will not rename onto an existing name.** It answers EIO (errno 5)
    // rather than replacing, which is measured on this board, not assumed — so
    // the old file has to go first, and the window between the two is real: a
    // power cut there leaves no `config.json` and a complete `config.json.new`.
    // `RecoverInterruptedWrite` closes that window at the next boot, which is
    // what keeps §10.15's promise that a write cannot half-happen.
    remove(final_path);
    if (rename(temp_path, final_path) != 0) {
        ESP_LOGE(TAG, "rename %s -> %s failed: errno %d", temp_path, final_path, errno);
        return ESP_FAIL;
    }
    return ESP_OK;
}

// The other half of the write above. Three states can be on the filesystem at
// boot, and only one of them is ambiguous:
//
//   * no `config.json.new` — nothing was interrupted;
//   * both files — the crash happened before the old one was removed, so the
//     temp is a leftover and the config is intact: drop the temp;
//   * only `config.json.new` — the crash landed in the window between the
//     remove and the rename. The temp is a *complete* file that never got its
//     name, so finishing the rename is the recovery, and restoring defaults
//     would throw away a good config for no reason.
void RecoverInterruptedWrite() {
    char temp_path[storage::kMaxPathLength];
    char final_path[storage::kMaxPathLength];
    if (!storage::ResolvePath(kTempPath, temp_path, sizeof(temp_path)) ||
        !storage::ResolvePath(kPath, final_path, sizeof(final_path))) {
        return;
    }

    FILE *temp = fopen(temp_path, "rb");
    if (temp == nullptr) {
        return;
    }
    fclose(temp);

    FILE *final_file = fopen(final_path, "rb");
    if (final_file != nullptr) {
        fclose(final_file);
        ESP_LOGW(TAG, "%s left over from an interrupted write; removed", kTempPath);
        remove(temp_path);
        return;
    }

    if (rename(temp_path, final_path) == 0) {
        ESP_LOGW(TAG, "%s was interrupted mid-replace; %s recovered from %s", kPath, kPath,
                 kTempPath);
    } else {
        ESP_LOGE(TAG, "%s exists but could not be renamed into place: errno %d", kTempPath, errno);
    }
}

}  // namespace

void FillDefaults(Data *out) {
    if (out == nullptr) {
        return;
    }
    *out = Data{};
    out->wifi.active = false;
    out->wifi.network_count = 0;
    snprintf(out->nats.url, sizeof(out->nats.url), "nats://192.168.1.5:4222");
    snprintf(out->time.zone, sizeof(out->time.zone), "UTC");
    snprintf(out->time.posix, sizeof(out->time.posix), "UTC0");
    snprintf(out->time.sntp_server, sizeof(out->time.sntp_server), "pool.ntp.org");
    out->display.brightness = 80;
    out->display.dim_seconds = 30;
    out->display.blank_seconds = 120;
    out->audio.volume_percent = 80;
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
    RecoverInterruptedWrite();

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
