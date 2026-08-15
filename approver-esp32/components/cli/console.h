#pragma once

// The device console (CLAUDE.md §10.7), on the USB Serial/JTAG port — the same
// port the monitor uses, which is why the two cannot both hold it.
//
// §10.7 specifies five commands (`register`, `keys`, `forget`, `bus`, `wifi`).
// Two exist so far, and they are the two that need nothing else to be built:
//
//   status         firmware, IDF and chip versions, the running slot, uptime,
//                  heap, and what the mounted filesystem is using
//   cat <path>     print a file from the `storage` partition (§10.15)
//
// It is built on ESP-IDF's own `esp_console` REPL rather than a hand-written
// line reader: history, editing and argument splitting come with it, and §10.4
// already approved the component.

#include "esp_err.h"

namespace console {

// Registers the commands and starts the REPL task. Call after storage::Init(),
// so `cat` has something to read.
esp_err_t Init();

}  // namespace console
