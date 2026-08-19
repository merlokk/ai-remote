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

// **`devstatus`, printed to whatever `stdout` currently is** (§10.16). It exists
// so that the web server can serve the same dump the console prints without a
// second copy of a single readout — §10.7's four-places rule says a second copy
// of the `power` section would drift from the first the day somebody adds a field
// to one of them, and a dump served over HTTP is exactly that temptation.
//
// It writes with `printf`, like every other readout here. The caller is what
// decides where that goes; `web_server.cpp` swaps `stdout` for the length of the
// call and says what that costs.
void PrintDevStatus();

}  // namespace console
