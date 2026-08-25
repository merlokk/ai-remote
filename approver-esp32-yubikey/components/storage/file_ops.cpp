#include <cerrno>
#include <cstdio>

#include "esp_log.h"
#include "storage.h"

// The file operations that have **no ESP-IDF in them** — `fopen`, `remove`,
// `rename` and the ordering between them (CLAUDE.md §10.15).
//
// They are in a file of their own for the reason `wifi_policy.cpp` and
// `sync_policy.cpp` are: this compiles against §10.11's fake platform with no
// change at all, so the atomic-write sequence that §10.15 spends most of its
// words on is the sequence the tests exercise rather than a second copy of it in
// `host_test/fakes/`. `storage.cpp` next door is all `esp_spiffs.h` and belongs
// to the device tier.
//
// It is still `storage`'s: `ResolvePath` is what turns a bare name into a path,
// and on the host that is the fake's version pointing at a scratch directory.

namespace storage {
namespace {

constexpr const char *TAG = "storage";

}  // namespace

esp_err_t WriteFileAtomically(const char *path, const char *temp_path, const char *contents,
                              size_t length) {
    char temp[kMaxPathLength];
    char final_path[kMaxPathLength];
    if (contents == nullptr || !ResolvePath(temp_path, temp, sizeof(temp)) ||
        !ResolvePath(path, final_path, sizeof(final_path))) {
        return ESP_ERR_INVALID_ARG;
    }

    FILE *file = fopen(temp, "wb");
    if (file == nullptr) {
        ESP_LOGE(TAG, "open %s failed: errno %d", temp, errno);
        return ESP_FAIL;
    }
    const size_t written = fwrite(contents, 1, length, file);
    // Both of these can fail on a full filesystem, and a close that failed is a
    // file that may not be on the medium — so the rename is not attempted.
    const bool flushed = fflush(file) == 0;
    const bool closed = fclose(file) == 0;
    if (written != length || !flushed || !closed) {
        ESP_LOGE(TAG, "write %s failed: %u of %u bytes, flush %d, close %d, errno %d", temp,
                 static_cast<unsigned>(written), static_cast<unsigned>(length), flushed, closed,
                 errno);
        remove(temp);
        return ESP_FAIL;
    }

    // SPIFFS will not rename onto an existing name — the header says why, and the
    // window this opens is what `RecoverInterruptedWrite` closes at the next boot.
    remove(final_path);
    if (rename(temp, final_path) != 0) {
        ESP_LOGE(TAG, "rename %s -> %s failed: errno %d", temp, final_path, errno);
        return ESP_FAIL;
    }
    return ESP_OK;
}

void RecoverInterruptedWrite(const char *path, const char *temp_path) {
    char temp[kMaxPathLength];
    char final_path[kMaxPathLength];
    if (!ResolvePath(temp_path, temp, sizeof(temp)) ||
        !ResolvePath(path, final_path, sizeof(final_path))) {
        return;
    }

    FILE *temp_file = fopen(temp, "rb");
    if (temp_file == nullptr) {
        return;
    }
    fclose(temp_file);

    FILE *final_file = fopen(final_path, "rb");
    if (final_file != nullptr) {
        fclose(final_file);
        ESP_LOGW(TAG, "%s left over from an interrupted write; removed", temp_path);
        remove(temp);
        return;
    }

    if (rename(temp, final_path) == 0) {
        ESP_LOGW(TAG, "%s was interrupted mid-replace; recovered from %s", path, temp_path);
    } else {
        ESP_LOGE(TAG, "%s exists but could not be renamed into place: errno %d", temp_path, errno);
    }
}

bool Exists(const char *path) {
    char resolved[kMaxPathLength];
    if (!ResolvePath(path, resolved, sizeof(resolved))) {
        return false;
    }
    FILE *file = fopen(resolved, "rb");
    if (file == nullptr) {
        return false;
    }
    fclose(file);
    return true;
}

esp_err_t Remove(const char *path) {
    char resolved[kMaxPathLength];
    if (!ResolvePath(path, resolved, sizeof(resolved))) {
        return ESP_ERR_INVALID_ARG;
    }
    // Nothing to remove is not a failure: §10.7's `forget` on a device that was
    // never registered has done exactly what was asked of it.
    if (remove(resolved) != 0 && errno != ENOENT) {
        ESP_LOGE(TAG, "remove %s failed: errno %d", resolved, errno);
        return ESP_FAIL;
    }
    return ESP_OK;
}

}  // namespace storage
