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

// SPIFFS stores names of at most `--obj-name-len` bytes, and the image is
// generated with 32 (see `spiffs_create_partition_image` in main/CMakeLists.txt).
inline constexpr size_t kMaxNameLength = 32;

// Mounts the partition. **Does not format on failure**, on purpose: formatting
// would throw away `config.init.json`, which is the one file §10.15's recovery
// path needs, and it would do it silently. A mount failure is reported and left
// alone — reflashing the image is the fix, and it is a fix that says so.
esp_err_t Init();

bool Mounted();

// Builds an absolute path in the caller's buffer, accepting what the operator
// is likely to type: with the mount point or without it. False when the result
// would not fit or the input is empty — never a truncated path, which would
// open the wrong file rather than fail.
//
// Public because playback streams a file itself rather than reading it whole
// (`audio::Speaker`), and one convention about what a path may look like beats
// two implementations of it.
bool ResolvePath(const char *path, char *out, size_t capacity);

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

// **A write that is not allowed to half-happen** (§10.15), and it is here rather
// than in `components/config` because there are two files that need it now — the
// settings and the registration of §10.7 — and the SPIFFS dance below is exactly
// the kind of thing that goes wrong the second time somebody writes it out.
//
// Temp file → remove the old → rename. The middle step is not tidiness:
// **SPIFFS will not rename onto an existing name.** It answers EIO (errno 5)
// rather than replacing, measured on this board rather than assumed. That leaves
// a real window in which the final name does not exist and a *complete* temp file
// is not yet called anything, which is what `RecoverInterruptedWrite` closes at
// the next boot.
//
// `temp_path` is the caller's rather than derived, so the name that appears in a
// `ls` after a crash is one the caller chose and can recognise.
esp_err_t WriteFileAtomically(const char *path, const char *temp_path, const char *contents,
                              size_t length);

// The other half of the write above, and it must run **before** the file is
// read. Three states can be on the filesystem at boot and only one is ambiguous:
//
//   * no temp — nothing was interrupted;
//   * both files — the crash happened before the old one was removed, so the temp
//     is a leftover and the real file is intact: drop the temp;
//   * only the temp — the crash landed in the window. The temp is a *complete*
//     file that never got its name, so finishing the rename is the recovery.
//     Restoring defaults here would throw away a good file to fix a naming
//     problem.
void RecoverInterruptedWrite(const char *path, const char *temp_path);

// True when the file exists. Cheaper to say than to read, and the question
// several callers actually have.
bool Exists(const char *path);

// Deletes it. `ESP_OK` when it is gone afterwards, whether or not it was there to
// begin with — nothing to forget is not a failure (§10.7's `forget`).
esp_err_t Remove(const char *path);

struct Entry {
    char name[kMaxNameLength];
    size_t size;
};

// Fills `out` with what is in the partition and writes how many entries were
// written to `*count`. The array is the caller's — nothing here allocates
// (§10.14.1).
//
// Returns `ESP_ERR_INVALID_SIZE` when there were more files than `capacity`,
// with the array full and `*count == capacity`: over capacity is a state the
// caller is told about, not one that silently looks like the whole listing.
//
// **There are no directories.** SPIFFS is flat, so this is the whole
// filesystem, not one level of it.
esp_err_t List(Entry *out, size_t capacity, size_t *count);

}  // namespace storage
