#pragma once

// The `storage` component, faked — but the filesystem underneath it is real.
//
// **That is the interesting choice here.** `config.cpp` does not go through
// `storage` to write: it resolves a path and then calls `fopen`, `remove` and
// `rename` itself, because the atomic-write dance of §10.15 is the whole point
// of that code. Faking those would mean testing a model of a filesystem
// instead of the sequence.
//
// So `ResolvePath` points at a real directory under the host's temp area and
// the real CRT does the rest. That is worth more than it sounds: **Windows
// `rename()` refuses to replace an existing file, exactly as SPIFFS does** —
// the EIO quirk §10.15 records measuring on the board. The remove-then-rename
// window, and the boot-time recovery that closes it, are therefore genuinely
// exercised rather than described.
//
// What is faked is the mount: `Mounted()` is a flag a test can clear, and
// `ReadFile` is the bounded read `storage` promises, so the "config is bigger
// than the buffer" path has something to fail against.

#include <cstddef>

#include "esp_err.h"

namespace fake {

// Creates an empty directory for this test and points `storage` at it. Called
// by `setUp` for the config suite; safe to call repeatedly, and each call
// wipes what the last one left.
void MountStorage();

// Makes `storage::Mounted()` false, so the "the filesystem never came up" path
// each of `config`'s entry points guards with has something to be false about.
void UnmountStorage();
bool IsMounted();

// The directory `storage::ResolvePath` resolves into.
const char *StoragePath();

// Put a file there / read one back / ask whether it is there. `contents` is a
// C string; the file gets exactly its bytes, with no terminator.
void PutFile(const char *name, const char *contents);

// The same, for content that is not text — a WAV is bytes, and half of
// them are zero.
void PutBinaryFile(const char *name, const void *data, size_t length);
bool FileExists(const char *name);

// Reads a file into `out`, NUL-terminated. Returns false if it is missing or
// does not fit. For asserting what was written rather than for driving code.
bool GetFile(const char *name, char *out, size_t capacity);

// Copies one of the repository's committed `spiffs_image/` files in. **The
// real file, not a copy of its text** — which is what makes "the shipped
// defaults parse" and "the two files have the same shape" tests mean something
// rather than test a fixture against itself.
bool PutRepoFile(const char *name);

}  // namespace fake
