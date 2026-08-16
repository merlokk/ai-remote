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

// "255.255.255.255" plus the terminator. Addresses are kept as **text**, the
// way they were typed: that is what `cat config.json` shows and what the
// console prints back, and an operator debugging a network wants to see the
// string they wrote rather than a number somebody re-rendered for them.
inline constexpr size_t kIpTextSize = 16;

// A fixed address for one network (§10.9). The house firmware of §10.14.4 has
// the same struct hanging off the same place — per *network*, not per device,
// and that is the half worth copying: a desk object that moves between a home
// with DHCP and an office that hands out nothing needs one of each, and a
// single device-wide setting would make the two mutually exclusive.
//
// Where it differs: theirs validates by checking the strings are non-empty and
// requires both DNS servers. Empty is not the way an address is usually wrong
// — `192.168.1.` and `192.168.1.256` are — so `ParseIpv4` below is what
// decides, and the DNS entries are optional.
struct StaticIp {
    bool enabled;
    char address[kIpTextSize];
    char netmask[kIpTextSize];
    char gateway[kIpTextSize];
    char dns1[kIpTextSize];  // empty is "don't set one"
    char dns2[kIpTextSize];
};

struct Network {
    char ssid[kSsidSize];
    char password[kPasswordSize];
    // DHCP unless this says otherwise, which is what `enabled` false means.
    StaticIp ip;
};

// Dotted quad to the 32-bit form `esp_netif_ip_info_t` wants — first octet in
// the **low** byte, which is what lwIP calls network order and what the
// console's own `%u.%u.%u.%u` already assumes.
//
// **Strict on purpose**, because this is the one place a typo can be caught
// while somebody is still looking at the screen: exactly four octets, decimal,
// 0..255, nothing before or after, and **no leading zeros** — `010` is ten
// here and eight to anything that reaches for `inet_aton`, and a device that
// silently lands on a different address than the one written down is a
// half-hour nobody gets back.
//
// Pure, and in the config layer rather than the driver, for the reason
// `tz::Lookup` is: turning what the file says into what the hardware takes is
// the file's job, and it makes both testable without a board.
bool ParseIpv4(const char *text, uint32_t *out);

// What the operator asked the radio to be (§10.9). Not what it is doing —
// that is `wifimgr`'s, and the difference between the two is the whole shape
// of that section. `active` false is "off" whatever this says, so there is one
// switch that means radio-down rather than two fields that can disagree.
enum class WifiMode : uint8_t {
    kClient = 0,  // join one of `networks`, trying each in turn
    kAp,          // be an access point, permanently
};

struct Wifi {
    bool active;
    WifiMode mode;
    Network networks[kMaxNetworks];
    uint8_t network_count;

    // How many full passes over `networks` before the device gives up being a
    // client and puts its own access point up instead (§10.9). 0 is read as 1.
    uint8_t rounds_before_ap;

    // How long that fallback access point stays up with nobody on it, before
    // going back to trying the list. Held open while a station is attached —
    // that station is the reason it exists.
    uint16_t ap_window_seconds;

    // The access point this device raises: for `mode: "ap"` and for the
    // fallback above, which are the same AP for different reasons.
    //
    // **An empty password is an open network**, which is what ships, and the
    // trade is stated in `config.init.json`. A password shorter than the eight
    // characters WPA2 requires is refused by the driver rather than silently
    // turned into an open one.
    char ap_ssid[kSsidSize];
    char ap_password[kPasswordSize];
    uint8_t ap_channel;
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

    // How often the clock is corrected from `sntp_server` (§10.8.2), in hours.
    // **Zero is off**, which is the one switch rather than a second boolean
    // that could disagree with it — the same call `Wifi::active` makes. An
    // empty `sntp_server` is off as well, and for the same reason: both are
    // the setting being absent rather than two settings arguing.
    //
    // It is a floor on the gap between *scheduled* syncs and not the whole
    // rule: a fresh boot and an internet that has just come back both sync at
    // once, whatever this says. `sync_policy.h` owns that, and this is the one
    // number of it the operator gets to set.
    uint8_t sync_hours;
};

// How many addresses the internet check may try (§10.9). Must match
// `wifimgr::kMaxProbeTargets`, which is asserted where the two meet.
inline constexpr size_t kMaxProbeTargets = 4;

// **Being associated is not being online**, and this is what tells the two
// apart: once a minute, while there is a link, ping one of these and see. A
// router with no uplink, a captive portal and a guest network that only allows
// port 80 all look like a healthy connection from the station's side.
//
// A **list**, not one address, for the reason 8.8.8.8 alone is not enough:
// plenty of otherwise usable networks drop ICMP to one operator or another,
// and one blocked host must not read as an outage.
struct InternetCheck {
    bool check;
    uint16_t interval_seconds;
    uint16_t timeout_ms;
    uint8_t failures_before_offline;
    char targets[kMaxProbeTargets][kIpTextSize];
    uint8_t target_count;
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
    InternetCheck internet;
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
