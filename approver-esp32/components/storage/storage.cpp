#include "storage.h"

#include <dirent.h>
#include <sys/stat.h>

#include <cstdio>
#include <cstring>

#include "esp_log.h"
#include "esp_spiffs.h"

namespace storage {

namespace {

constexpr const char *TAG = "storage";

bool mounted = false;

// Builds an absolute path in the caller's buffer. Accepts what the operator is
// likely to type: with the mount point or without it.
bool BuildPath(const char *path, char *out, size_t capacity) {
    if (path == nullptr || path[0] == '\0') {
        return false;
    }
    const int written = (path[0] == '/') ? snprintf(out, capacity, "%s", path)
                                         : snprintf(out, capacity, "%s/%s", kBasePath, path);
    return written > 0 && static_cast<size_t>(written) < capacity;
}

}  // namespace

esp_err_t Init() {
    const esp_vfs_spiffs_conf_t conf = {
        .base_path = kBasePath,
        .partition_label = kPartitionLabel,
        .max_files = kMaxOpenFiles,
        .format_if_mount_failed = false,
    };

    const esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err != ESP_OK) {
        // Deliberately not formatted away: see the header. The image is flashed
        // with the project, so a failure here means the flash does not hold
        // what the build produced.
        ESP_LOGE(TAG, "mount of '%s' failed: %s", kPartitionLabel, esp_err_to_name(err));
        return err;
    }

    mounted = true;

    size_t total = 0;
    size_t used = 0;
    if (Info(&total, &used) == ESP_OK) {
        ESP_LOGI(TAG, "mounted at %s, %u of %u bytes used", kBasePath,
                 static_cast<unsigned>(used), static_cast<unsigned>(total));
    } else {
        ESP_LOGI(TAG, "mounted at %s", kBasePath);
    }
    return ESP_OK;
}

bool Mounted() { return mounted; }

esp_err_t Info(size_t *total_bytes, size_t *used_bytes) {
    return esp_spiffs_info(kPartitionLabel, total_bytes, used_bytes);
}

esp_err_t ReadFile(const char *path, char *out, size_t capacity, size_t *length) {
    if (out == nullptr || capacity == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!mounted) {
        return ESP_ERR_INVALID_STATE;
    }

    char full[kMaxPathLength];
    if (!BuildPath(path, full, sizeof(full))) {
        return ESP_ERR_INVALID_ARG;
    }

    FILE *file = fopen(full, "rb");
    if (file == nullptr) {
        return ESP_ERR_NOT_FOUND;
    }

    // The size first, so a file that does not fit is refused whole rather than
    // handed back truncated and looking complete.
    fseek(file, 0, SEEK_END);
    const long size = ftell(file);
    rewind(file);

    if (size < 0) {
        fclose(file);
        return ESP_FAIL;
    }
    if (static_cast<size_t>(size) + 1 > capacity) {
        fclose(file);
        if (length != nullptr) {
            *length = static_cast<size_t>(size);
        }
        return ESP_ERR_INVALID_SIZE;
    }

    const size_t read = fread(out, 1, static_cast<size_t>(size), file);
    fclose(file);

    out[read] = '\0';
    if (length != nullptr) {
        *length = read;
    }
    return (read == static_cast<size_t>(size)) ? ESP_OK : ESP_FAIL;
}

esp_err_t List(Entry *out, size_t capacity, size_t *count) {
    if (out == nullptr || capacity == 0 || count == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    *count = 0;
    if (!mounted) {
        return ESP_ERR_INVALID_STATE;
    }

    DIR *dir = opendir(kBasePath);
    if (dir == nullptr) {
        return ESP_ERR_NOT_FOUND;
    }

    bool overflowed = false;
    for (const dirent *entry = readdir(dir); entry != nullptr; entry = readdir(dir)) {
        if (*count >= capacity) {
            // Keep counting nothing — the caller is told it did not see
            // everything, which is the honest half of a fixed-size listing.
            overflowed = true;
            break;
        }

        Entry &slot = out[*count];
        // `d_name` is 256 bytes and our slot is 32, so the bound is explicit:
        // snprintf would truncate correctly but the compiler is right to call
        // that a silent shortening, and -Werror agrees.
        const size_t name_length = strnlen(entry->d_name, sizeof(slot.name) - 1);
        memcpy(slot.name, entry->d_name, name_length);
        slot.name[name_length] = '\0';

        // `dirent` carries no size on this filesystem, so ask separately. A
        // file that vanishes between the two is listed with size 0 rather than
        // dropped.
        slot.size = 0;
        char full[kMaxPathLength];
        if (BuildPath(slot.name, full, sizeof(full))) {
            struct stat info = {};
            if (stat(full, &info) == 0) {
                slot.size = static_cast<size_t>(info.st_size);
            }
        }

        ++(*count);
    }

    closedir(dir);
    return overflowed ? ESP_ERR_INVALID_SIZE : ESP_OK;
}

}  // namespace storage
