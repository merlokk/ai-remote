#pragma once

// SNTP — the other half of the clock (CLAUDE.md §10.8.2).
//
// The PCF85063 is the time source at boot: instant, offline, and right across a
// power cut. What it cannot do is be *accurate* — it is a watch crystal, it
// drifts, and nothing on the board can tell by how much. So when there is a
// network, the device asks a server, sets the system clock and writes the
// answer back to the chip, and the RTC is correct again for the next boot.
//
// Everything about **when** is next door in `sync_policy.h`, which includes
// `<cstdint>` and nothing else and is therefore entirely testable on the host
// (§10.11). This file is the part with no decisions in it: read whether there
// is an internet, run one exchange, write what came back.
//
// **It reads `wifimgr` rather than asking the network itself** — the seam
// §10.9 said SNTP would hang off. A second probe with an opinion of its own
// would be a second answer to a question the device already answers once.
//
// Two rules it inherits, both from §10.8.2:
//
//   * **the clock is UTC and a zone is presentation.** What arrives is UTC,
//     what goes into the RTC is UTC, and nothing here so much as reads the
//     configured zone;
//   * **an obviously unset clock beats a plausible wrong one.** An answer
//     outside the years this RTC can hold is refused rather than stored, and
//     counted as a failed sync.

#include <ctime>

#include "esp_err.h"
#include "pcf85063.h"
// For `kNever`, the value the ages below carry when there is nothing to age.
// The policy itself is nobody's business but this component's; the constant is
// part of what `Status` means.
#include "sync_policy.h"

namespace timesync {

// What a console line — and, later, §10.8.2's clock face — needs, taken at one
// instant.
struct Status {
    bool enabled = false;

    // What the manager reports, in the one form this component cares about:
    // is there something to ask through. `false` is "no link" and "the check
    // says offline" alike, which are different facts to §10.9 and the same
    // fact here.
    bool internet = false;

    bool syncing = false;
    bool ever_synced = false;

    // Ages, not timestamps — `kNever` when there is nothing to age (see
    // `sync_policy.h`).
    uint32_t since_last_ms = 0;
    uint32_t next_in_ms = 0;

    uint16_t successes = 0;
    uint16_t failures = 0;

    // The wall-clock moment of the last successful sync, and how far the clock
    // moved when it happened. The second is the number worth having: a device
    // whose every sync steps it by four seconds has an RTC to be suspicious of,
    // and nothing else on this board would ever say so.
    time_t last_utc = 0;
    int32_t last_step_seconds = 0;

    esp_err_t last_error = ESP_OK;
};

// Starts the sync task. Does **not** ask anything: what happens next is
// whatever `config.json` asks for and whatever the network turns out to be.
// The clock is borrowed, not owned — `main` passes the board's, which keeps
// this component free of `board.h` (§10.14.2).
esp_err_t Init(rtc::Pcf85063 *clock);
bool Ready();

// Re-reads the interval out of `config.json` and nothing else. The narrowest
// thing that changed, which is the lesson §10.9 records the hard way: a
// settings call that throws away a good connection — or here, a good sync — is
// a settings call people stop making.
void Apply();

// Ask now instead of at the next interval — the console's `date sync`. Does
// nothing while there is no internet to ask through, and nothing at all when
// syncing is switched off.
void SyncNow();

Status Get();

}  // namespace timesync
