#pragma once

// The `storage` partition, mounted (CLAUDE.md §10.15). Library layer: it knows
// about files, and nothing about approvals (§10.14.2).
//
// The image is built from `spiffs_image/` and flashed with the project, so a
// freshly flashed device already has `config.json` and `config.init.json` in
// here. SPIFFS is flat — it has no directories — so paths are `/spiffs/name`
// and nothing deeper.

#include <cstddef>

#include "esp_err.h"

namespace storage {

inline constexpr const char *kPartitionLabel = "storage";
inline constexpr const char *kBasePath = "/spiffs";
inline constexpr size_t kMaxOpenFiles = 4;

// Longest absolute path this layer will build, base path included.
inline constexpr size_t kMaxPathLength = 64;

// Mounts the partition. **Does not format on failure**, on purpose: formatting
// would throw away `config.init.json`, which is the one file §10.15's recovery
// path needs, and it would do it silently. A mount failure is reported and left
// alone — reflashing the image is the fix, and it is a fix that says so.
esp_err_t Init();

bool Mounted();

// Bytes the filesystem accounts for. `total` is smaller than the partition:
// SPIFFS keeps its own metadata.
esp_err_t Info(size_t *total_bytes, size_t *used_bytes);

// Reads a whole file into `out` and NUL-terminates it. `path` may be absolute
// (`/spiffs/config.json`) or bare (`config.json`).
//
// Returns `ESP_ERR_INVALID_SIZE` when the file does not fit in `capacity`,
// writing the size it would need to `*length` — the caller decides what to do
// about it rather than getting a truncated file that looks whole. On success
// `*length` is the byte count, not counting the terminator. `length` may be
// null. Nothing here allocates (§10.14.1): the buffer is the caller's.
esp_err_t ReadFile(const char *path, char *out, size_t capacity, size_t *length);

}  // namespace storage
