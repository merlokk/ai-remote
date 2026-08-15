#include "fake_storage.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

#include "storage.h"

// `SPIFFS_IMAGE_DIR` is passed by CMake and points at the repository's
// `spiffs_image/`. It is how `PutRepoFile` reaches the committed files.
#ifndef SPIFFS_IMAGE_DIR
#define SPIFFS_IMAGE_DIR ""
#endif

namespace fake {

namespace {

std::filesystem::path root;
bool mounted = false;

std::filesystem::path Full(const char *name) {
    // Accept both spellings, the same way `storage::ResolvePath` does.
    const char *bare = name;
    const size_t base_length = std::strlen(storage::kBasePath);
    if (std::strncmp(name, storage::kBasePath, base_length) == 0 && name[base_length] == '/') {
        bare = name + base_length + 1;
    }
    return root / bare;
}

}  // namespace

void MountStorage() {
    if (root.empty()) {
        // **Relative, and short on purpose.** `storage::kMaxPathLength` is 64
        // and `ResolvePath` refuses rather than truncating — which is the right
        // behaviour and which the host's temp directory
        // (`C:\Users\…\AppData\Local\Temp\…`) blows straight past. A fake that
        // worked around that by widening the buffer would be a fake that hides
        // the constraint the device actually has. `build/` is git-ignored, and
        // `run.cmd` makes `host_test/` the working directory.
        root = std::filesystem::path("build") / "fs";
    }
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root, ec);
    mounted = true;
}

void UnmountStorage() { mounted = false; }

const char *StoragePath() {
    static std::string cached;
    cached = root.string();
    return cached.c_str();
}

bool IsMounted() { return mounted; }

void PutFile(const char *name, const char *contents) {
    FILE *file = std::fopen(Full(name).string().c_str(), "wb");
    if (file == nullptr) {
        return;
    }
    std::fwrite(contents, 1, std::strlen(contents), file);
    std::fclose(file);
}

bool FileExists(const char *name) {
    std::error_code ec;
    return std::filesystem::exists(Full(name), ec);
}

bool GetFile(const char *name, char *out, size_t capacity) {
    FILE *file = std::fopen(Full(name).string().c_str(), "rb");
    if (file == nullptr) {
        return false;
    }
    const size_t read = std::fread(out, 1, capacity - 1, file);
    const bool more = std::fgetc(file) != EOF;
    std::fclose(file);
    if (more) {
        return false;
    }
    out[read] = '\0';
    return true;
}

bool PutRepoFile(const char *name) {
    const std::filesystem::path source = std::filesystem::path(SPIFFS_IMAGE_DIR) / name;
    std::error_code ec;
    std::filesystem::copy_file(source, Full(name),
                               std::filesystem::copy_options::overwrite_existing, ec);
    return !ec;
}

}  // namespace fake

// --- The `storage` component's own surface --------------------------------

namespace storage {

esp_err_t Init() {
    fake::MountStorage();
    return ESP_OK;
}

bool Mounted() { return fake::IsMounted(); }

bool ResolvePath(const char *path, char *out, size_t capacity) {
    if (path == nullptr || path[0] == '\0') {
        return false;
    }
    const char *bare = path;
    const size_t base_length = std::strlen(kBasePath);
    if (std::strncmp(path, kBasePath, base_length) == 0 && path[base_length] == '/') {
        bare = path + base_length + 1;
    }

    const int written = std::snprintf(out, capacity, "%s/%s", fake::StoragePath(), bare);
    // Never a truncated path — the real one makes the same promise, and a
    // truncated path opens the wrong file instead of failing.
    return written > 0 && static_cast<size_t>(written) < capacity;
}

esp_err_t Info(size_t *total_bytes, size_t *used_bytes) {
    if (total_bytes != nullptr) {
        *total_bytes = 1024 * 1024;
    }
    if (used_bytes != nullptr) {
        *used_bytes = 0;
    }
    return ESP_OK;
}

esp_err_t ReadFile(const char *path, char *out, size_t capacity, size_t *length) {
    char full[kMaxPathLength];
    if (!ResolvePath(path, full, sizeof(full))) {
        return ESP_ERR_INVALID_ARG;
    }

    FILE *file = std::fopen(full, "rb");
    if (file == nullptr) {
        return ESP_ERR_NOT_FOUND;
    }
    std::fseek(file, 0, SEEK_END);
    const long size = std::ftell(file);
    std::fseek(file, 0, SEEK_SET);

    if (size < 0 || static_cast<size_t>(size) >= capacity) {
        // The real one reports the size it would have needed rather than
        // handing back a truncated file that looks whole.
        if (length != nullptr && size >= 0) {
            *length = static_cast<size_t>(size);
        }
        std::fclose(file);
        return ESP_ERR_INVALID_SIZE;
    }

    const size_t read = std::fread(out, 1, static_cast<size_t>(size), file);
    std::fclose(file);
    out[read] = '\0';
    if (length != nullptr) {
        *length = read;
    }
    return ESP_OK;
}

esp_err_t List(Entry *out, size_t capacity, size_t *count) {
    (void)out;
    (void)capacity;
    if (count != nullptr) {
        *count = 0;
    }
    return ESP_OK;
}

}  // namespace storage
