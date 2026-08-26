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
// Pure, and in the config layer rather than the driver: turning what the file
// says into what the hardware takes is the file's job, and it makes both
// testable without a board.
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
    // **Whether that AP is protected is this field's answer, not the
    // firmware's**: empty raises an open network, eight characters or more
    // raise a WPA2 one, and anything in between is refused by the driver
    // rather than silently turned into an open AP somebody believes is locked.
    //
    // The two shipped files deliberately differ — `config.json` sets a key and
    // `config.init.json` does not — so a restore opens the access point.
    // §10.9 has the argument.
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

// --- The one output this device has (§10.1, §10.17) -----------------------
//
// One WS2812 on GPIO48 says everything this device has to say — the colour is the
// state and the rhythm is the urgency (§10.17 is the whole table). Two numbers are
// the operator's,
// and both are brightness rather than colour for a reason worth writing down:
// **which colour means what is protocol, not preference.** An operator who can
// recolour "denied" can build a device that lies about what it did, so the
// palette is compiled in and this struct cannot reach it.
//
// A WS2812 at full scale on a desk at night is a torch, which is the whole
// reason `idle_percent` exists apart from `percent`: the resting breath of a
// device that is doing nothing should be findable in a dark room and not read
// from across it. A request overrides it — §10.17's rule is that the pending
// state always uses `percent`, because a request nobody notices is a request
// that times out.
struct Led {
    uint8_t percent;       // 0..100, the ceiling every colour is scaled to
    uint8_t idle_percent;  // 0..100, what the ready-state breath settles to
};

// **When a verdict may be asked for** (§10.18). Two fields, and neither of them
// can decide *what* the verdict is.
//
// **There is no `requireKey` here and there cannot be one.** It existed while the
// device signed with a key of its own and could therefore be told to skip the
// security key; since §10.18 the derived private key lives inside the
// authenticator and nowhere else, so a device with no key is not a device with a
// looser policy — it is a device with no signature to make. A setting that
// pretended otherwise would be a switch that does nothing, which is worse than an
// absent one.
struct Approval {
    // How long a pending request waits for that touch before the device gives
    // up on it. Nothing is published when it expires — §10.10's fail-safe is
    // silence, and the hook's own timeout is what closes the loop.
    //
    // Shorter than the hook's timeout on purpose: a device that is still
    // waiting after the asker has stopped listening is a light that means
    // nothing.
    uint16_t touch_timeout_seconds;

    // Whether a short press on BOOT is a `deny` (§10.1). **The only thing the
    // button can produce is a deny** — there is no field here that would let it
    // produce the other one, which is §10.10 expressed as a missing feature
    // rather than as a check.
    bool deny_button;
};

struct Data {
    Wifi wifi;
    InternetCheck internet;
    Nats nats;
    Led led;
    Approval approval;
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

// Copies `config.init.json` over `config.json` and re-reads it — the same thing
// holding `BOOT` at boot does (§10.15), and what `config restore` on the console
// calls. `registration.json` is not touched.
esp_err_t Restore();

// --- Who has to be told when the fields moved under them ------------------
//
// A `Reload` or a `Restore` replaces every field at once, and four subsystems are
// holding copies of some of them: the light has a brightness, the Wi-Fi manager a
// network list, the bus a URL, the gate a timeout. Telling them is not this
// component's job — it has never heard of a radio (§10.14.2) — but *remembering*
// to tell them cannot be the caller's, because there are two callers: the
// console's `config reload` and the restore.
//
// **So it is a hook, and `nats`'s is the other one**: `main` registers what has to
// happen, and `Reload`/`Restore` call it themselves on success — which is what
// makes a reload typed on the console and a restore the same reload rather than
// two lists somebody has to keep in step.
//
// **`Init` deliberately does not call it.** It runs before any of those
// subsystems exists, and `main` applies each of them explicitly in an order that
// is written down there.
using ChangeHandler = void (*)();
void OnChanged(ChangeHandler handler);

// --- `BOOT` after boot (§10.15) -------------------------------------------
//
// How long the button has to be held. **Long on purpose**: five seconds is past
// anything somebody crosses by accident while plugging a cable in.
//
// **And it is held *after* the device is running, not through the reset** —
// which is the one place this board's restore differs from the C6 one's and the
// difference is not a preference. `BOOT` is GPIO0, and GPIO0 held across a reset
// is what puts the ROM into download mode; a restore that asked for that would
// be a restore nobody could perform. So the window opens once `app_main` is
// running: the LED says so (§10.17 — white, fast), and the firmware samples the
// button then. There is feedback for this hold, which the C6's five blind
// seconds never had.
inline constexpr uint32_t kRestoreHoldMs = 5000;

enum class RestoreOutcome : uint8_t {
    kNotRequested = 0,  // nobody held it: nothing was read and nothing written
    kRestored,          // `config.init.json` is now `config.json`
    kFailed,            // asked for and did not happen — the settings are as they were
};

// The restore §10.15 gives `BOOT` its second job for. **Whether the button was held is
// the caller's answer**, not this component's: a layer that knows about a file
// and its fields has never heard of a GPIO (§10.14.2), and `main` is where the
// two meet — the same place the codec's volume is applied.
//
// **It runs before `Init()`, and that ordering is the whole point.** The failure
// this button exists for is a config that stops the device booting; a restore
// that ran after the parse could not rescue it. So: this writes the file, and
// `Init()` then reads it like any other boot.
//
// A failure changes nothing on the filesystem — a missing `config.init.json` or
// an unmounted partition leaves the settings that are there exactly where they
// are, because destroying a working config to report a missing default file is
// the worst of both. It does leave the built-in defaults in memory, which the
// `Init()` that follows overwrites with whatever the file really says.
RestoreOutcome RestoreAtBoot(bool key_held);

// What the last `RestoreAtBoot` decided, for anything that has to say so later.
RestoreOutcome BootRestore();

// That, as one line for the operator, or null when there is nothing to say.
// **One string rather than two**: the boot log and the console answer the same
// question, and a second copy of a sentence is a copy that drifts.
const char *BootRestoreText();

// The live values. Mutable on purpose: a caller changes a field and calls
// `Save()`. There is exactly one of these, for the life of the device.
Data &Get();

// The compiled-in fallback, used when even `config.init.json` cannot be read.
// It is not the source of truth — the file is (§10.15) — but a device with an
// unreadable filesystem should still boot with sane numbers rather than zeros.
void FillDefaults(Data *out);

}  // namespace config
