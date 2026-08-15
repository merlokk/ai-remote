#pragma once

// `config.json` on the storage partition, parsed into fields (CLAUDE.md §10.15).
//
// **One struct, fixed size, no heap** (§10.14.1). Everything the operator can
// set lives here; the file is read once at boot, can be re-read, and is written
// back atomically. What is deliberately *not* here is the registration
// (§10.7) — it lives in its own file precisely so that restoring settings
// cannot cost a token.
//
// Three rules from §10.15 that this component implements rather than describes:
//
//   * a `config.json` that is missing, oversized or unparseable is **restored
//     from `config.init.json`**, with one log line, and boot continues. Bad
//     input is recovered from, never a reboot loop;
//   * a write goes to `config.json.new` and is then renamed over the original,
//     because a power cut in the middle of the recovery path is the one failure
//     that would break recovery itself;
//   * **unknown fields are ignored and lost on the next write.** That is the
//     honest behaviour of a fixed struct: a file written by a newer firmware
//     does not survive a downgrade. Worth knowing before it surprises someone.
//
// Library layer (§10.14.2): it knows about a file and its fields, and nothing
// about approvals.

#include <cstddef>
#include <cstdint>

#include "esp_err.h"

namespace config {

inline constexpr const char *kPath = "config.json";
inline constexpr const char *kDefaultsPath = "config.init.json";
inline constexpr const char *kTempPath = "config.json.new";

// Cap the file before parsing (§10.15). Generous next to the ~1 KB this holds,
// and small next to the 4 KB the console already reserves for `cat`.
inline constexpr size_t kMaxFileSize = 4096;

// §10.9: "a desk device that moves between a home and an office is the entire
// use case; roaming between dozens is not."
inline constexpr size_t kMaxNetworks = 4;

inline constexpr size_t kSsidSize = 33;      // 32 bytes + terminator
inline constexpr size_t kPasswordSize = 65;  // 64 bytes + terminator
inline constexpr size_t kUrlSize = 64;
inline constexpr size_t kTimezoneSize = 48;  // POSIX TZ, e.g. CET-1CEST,M3.5.0,M10.5.0/3
inline constexpr size_t kHostSize = 64;

struct Network {
    char ssid[kSsidSize];
    char password[kPasswordSize];
};

struct Wifi {
    bool active;
    Network networks[kMaxNetworks];
    uint8_t network_count;
};

// Named after what it is rather than after its role: there is exactly one bus
// here and it is NATS (§10.3), so `nats.url` reads as an address and `bus.url`
// read as an abstraction with one implementation.
struct Nats {
    // The server on the LAN. No credentials: that bus has none, and the day it
    // does this grows a field rather than a design.
    char url[kUrlSize];
};

struct Time {
    // **Two fields for one setting, and the pair is the point** (§10.8.2; the
    // house firmware of §10.14.4 keeps the same two): `zone` is what a person
    // reads and types — `Europe/Kyiv` — and `posix` is the rule libc is
    // actually given. Keeping both means a zone whose transitions moved can be
    // corrected on the device by writing `posix` alone, without waiting for a
    // firmware whose table knows the new rule.
    //
    // Neither of them moves the clock. The RTC and `time_t` are UTC, always;
    // a zone is how a time is shown and how a typed time is read, never how it
    // is stored.
    char zone[kTimezoneSize];
    char posix[kTimezoneSize];
    char sntp_server[kHostSize];
};

struct Display {
    uint8_t brightness;      // percent
    uint16_t dim_seconds;    // idle before dimming; 0 disables
    uint16_t blank_seconds;  // idle before the panel blanks; 0 disables
};

struct Audio {
    uint8_t volume_percent;  // what the codec is set to at boot
};

struct Data {
    Wifi wifi;
    Nats nats;
    Time time;
    Display display;
    Audio audio;
};

// Reads `config.json` into the fields, restoring it from `config.init.json`
// first if it is missing, too big or not parseable. Storage must be mounted.
esp_err_t Init();

bool Loaded();

// Re-reads the file into the fields, discarding anything set since. Unlike
// `Init` this does **not** restore from the defaults on a bad file: a reload
// is somebody asking what the file says, and answering by overwriting it would
// destroy the thing they were asking about.
esp_err_t Reload();

// Writes the fields back, atomically (temp file, then rename).
esp_err_t Save();

// Copies `config.init.json` over `config.json` and re-reads it — the same
// thing holding `KEY` at boot does (§10.15), and what the settings screen's
// "Restore config" entry will call. `registration.json` is not touched.
esp_err_t Restore();

// The live values. Mutable on purpose: a caller changes a field and calls
// `Save()`. There is exactly one of these, for the life of the device.
Data &Get();

// The compiled-in fallback, used when even `config.init.json` cannot be read.
// It is not the source of truth — the file is (§10.15) — but a device with an
// unreadable filesystem should still boot with sane numbers rather than zeros.
void FillDefaults(Data *out);

}  // namespace config
