#pragma once

// The device console (CLAUDE.md §10.7), on **UART0 — the CH343P bridge**, which
// is the board's first USB-C socket. The second one is the chip's own USB and is
// a *host* for a security key (§10.18.4), so unlike the sibling board these two
// jobs do not have to share a port.
//
// It is still the same port the monitor uses, which is why the two cannot both
// hold it.
//
// **[`commands.md`](../../commands.md) is the list of what you can type** — every
// command, every subcommand, and what each one does. It is not repeated here: two
// copies of a command list means one of them is wrong.
//
// It is built on ESP-IDF's own `esp_console` REPL rather than a hand-written line
// reader: history, editing and argument splitting come with it, and §10.4 already
// approved the component.

#include "esp_err.h"

namespace console {

// Registers the commands and starts the REPL task. Call after storage::Init(),
// so `cat` has something to read.
esp_err_t Init();

// **`devstatus`, printed to whatever `stdout` currently is.** On the sibling
// board this exists so its web server can serve the same dump the console prints
// without a second copy of a single readout; **there is no web server here**, so
// today it has exactly one caller — the `devstatus` command itself.
//
// It is kept exported anyway, and that is a decision rather than an oversight:
// §10.7's four-places rule is about copies of a readout, and the moment a second
// surface wants this dump the temptation is to write it again. One function,
// however many callers.
void PrintDevStatus();

}  // namespace console
