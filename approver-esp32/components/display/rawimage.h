#pragma once

// A full-screen image, streamed off the filesystem straight to the panel
// (CLAUDE.md §10.8's splash, and anything else that wants the glass before
// LVGL owns it).
//
// Library layer: it knows about pixels and files, and nothing about what the
// picture means (§10.14.2). The caller decides which file and for how long.
//
// **No decoder, and the argument is `speaker.h`'s.** The firmware plays
// uncompressed WAV because a decoder is a dependency, a heap and a class of
// failure for something a host-side tool can do once at build time; the same
// reasoning gives this a headerless raw format. `tools/make-splash.ps1`
// produces it and `working-with-code.md` has the command.
//
// The format, stated once so a wrong file is a wrong file rather than a
// mystery:
//
//   * **raw RGB565, big-endian, no header** — the panel's own byte order, so
//     the bytes go out untouched. The swap LVGL is told to do with
//     `flags.swap_bytes` happens here at build time instead;
//   * exactly `width * height * 2` bytes for the panel it is drawn on. A file
//     of any other size is **refused with its size**, never stretched or
//     padded — a splash drawn from a truncated file is a panel full of noise
//     that looks like a driver fault.
//
// It streams in strips through one fixed buffer (§10.14.1 — nothing here
// allocates), which is also why it costs no more RAM for a 460 KB image than
// for a small one.

#include "esp_err.h"
#include "panel.h"

namespace display {

// The rows one strip carries. Small on purpose: this buffer is static and
// lives for the life of the device (§10.14.1), so it is sized for what a boot
// splash needs rather than for throughput. Eight rows of 480 is 7 680 bytes
// and about sixty reads for a full screen — a third of a second off SPIFFS,
// which for a picture that is then held on screen is not a number worth
// spending 38 KB of SRAM to improve.
inline constexpr int kStripLines = 8;

// `path` may be absolute (`/spiffs/splash.bin`) or bare (`splash.bin`), the
// same convention `storage::ResolvePath` gives everything else.
//
// `ESP_ERR_NOT_FOUND` if it is not there, `ESP_ERR_INVALID_SIZE` if it is the
// wrong size for this panel, `ESP_ERR_INVALID_STATE` if the panel is not up.
esp_err_t BlitRaw(Panel &panel, const char *path);

}  // namespace display
