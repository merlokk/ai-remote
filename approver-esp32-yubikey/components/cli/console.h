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

// **`devstatus`, printed to whatever `stdout` currently is.** It exists so the
// web server of §10.16 can serve the same dump the console prints without a
// second copy of a single readout, and it now has the second caller it was
// exported for: `console::Init` hands this function to `web::SetDiagnostics`, and
// `GET /api/devstatus` swaps `stdout` for the length of the call.
//
// **The hook runs that way round on purpose.** `cli` depends on `web` — the `web`
// command reads its status — so `web` must never depend on `cli`, and a cycle in
// `REQUIRES` is a build that does not happen. One function, two surfaces, and
// §10.7's four-places rule kept: the moment a second surface wants a readout, the
// temptation is to write it again.
void PrintDevStatus();

}  // namespace console
