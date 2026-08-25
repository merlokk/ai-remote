#pragma once

// The radio, and nothing else (CLAUDE.md §10.9).
//
// **This driver has no opinions.** It joins the network it is told to join, or
// becomes an access point, says what the link is doing, and lists what is on
// the air. Which network, when to give up, and when to stop being a client and
// start being findable are all somebody else's decisions — they live in
// `components/wifimgr`, whose `Policy` includes nothing and is therefore
// testable on the host. §10.14.2's split, applied to the one piece of hardware
// on this board that is inside the chip.
//
// Everything here is the other half of that split: `esp_wifi`, `esp_netif`,
// `esp_event`, and the mapping from ESP-IDF's disconnection reason codes onto
// the three answers a person can act on. None of it can run without a radio,
// so none of it is in §10.11's host tier — the tests it owes are device-tier
// (§10.11 tier 3), and the two rules worth writing there are that a wrong
// password is reported as a wrong password and that a scan does not upset a
// connection.
//
// Three things it does that are decisions rather than plumbing, each argued at
// its implementation:
//
//   * **credentials are never given to the driver's own store**
//     (`WIFI_STORAGE_RAM`, §10.9): there is exactly one record of what this
//     device knows and it is `config.json`, so one restore clears it;
//   * **an access point here is `APSTA`, not `AP`** — the station interface
//     stays up and unconnected so that a scan works while the fallback AP is
//     the only thing running, which is precisely the moment somebody needs to
//     pick a network;
//   * **a disconnection this code asked for is not a failure**, and the
//     suppression counter that makes that true is the subtlest thing in the
//     file.

#include <cstddef>
#include <cstdint>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace wifi {

// Same numbers as `config::kSsidSize` / `kPasswordSize`, and the same reason:
// 802.11 puts 32 bytes in an SSID and 63 characters in a WPA passphrase.
inline constexpr size_t kSsidSize = 33;
inline constexpr size_t kPasswordSize = 65;

// What one scan may return. Sixteen APs is a crowded flat and about 1.5 KB of
// static `wifi_ap_record_t`, which is the number that decides it — nothing
// here allocates (§10.14.1), so this array exists for the life of the device
// whether or not anybody ever scans.
inline constexpr size_t kMaxScanResults = 16;

// The shortest WPA2 passphrase the standard allows. A shorter one is refused
// rather than quietly turned into an open access point.
inline constexpr size_t kMinApPasswordLength = 8;

enum class Mode : uint8_t {
    kOff,
    kClient,  // station: joining, or joined to, one network
    kAp,      // access point (plus an idle station interface — see above)
};

enum class Link : uint8_t {
    kIdle,        // the radio is up but not trying to be anywhere
    kConnecting,  // an association is in flight
    kConnected,   // joined, and holding an IPv4 address
    kFailed,      // **latched**: it stays until the next Start…, so a poller
                  // cannot miss the edge by looking a moment too late
};

// §10.9: "wrong password and 'the AP is out of range' are different problems
// and the screen must not spell them the same way". The raw reason code is
// kept beside this for the log; this is the part anything else may branch on.
enum class Failure : uint8_t {
    kNone,
    kAuth,
    kNotFound,
    kOther,
};

// A fixed address for the network being joined (§10.9), in the binary form
// `esp_netif_ip_info_t` takes — first octet in the low byte. The **text** lives
// in `config.json` and turning one into the other is `config::ParseIpv4`'s job:
// this driver is handed an address the same way it is handed an SSID, and has
// no idea where either came from (§10.14.2).
//
// A zero DNS entry means "do not set one", which is different from 0.0.0.0 and
// is the only value that could not be a real server anyway.
struct StaticIp {
    bool enabled = false;
    uint32_t address = 0;
    uint32_t netmask = 0;
    uint32_t gateway = 0;
    uint32_t dns1 = 0;
    uint32_t dns2 = 0;
};

struct Status {
    Mode mode = Mode::kOff;
    Link link = Link::kIdle;
    Failure failure = Failure::kNone;

    // The raw `wifi_err_reason_t`. Not interpreted anywhere but in the log and
    // the console: the classification above is what code reads.
    uint8_t reason = 0;

    // The network being tried or joined; in AP mode, the one being served.
    char ssid[kSsidSize] = {};

    int8_t rssi = 0;

    // **The channel the radio is actually tuned to**, read back rather than
    // assumed — 0 when it is not on one. Worth having as its own field because
    // it can disagree with the configured one: an access point raised while
    // the station is associated is forced onto the station's channel, so
    // `wifi.ap.channel` in `config.json` is a request and this is the answer.
    uint8_t channel = 0;
    uint32_t ip = 0;       // IPv4, host order via esp_ip4_addr_t; 0 when none
    bool ip_is_static = false;  // …and where it came from, which a readout
                                // needs: "it has an address" and "the address
                                // is the one that was asked for" differ
    uint8_t clients = 0;   // stations attached to our AP
    uint32_t changes = 0;  // bumped on every transition, for a poller
};

struct ScanResult {
    char ssid[kSsidSize];
    int8_t rssi;
    uint8_t channel;
    bool secured;
};

class Radio {
   public:
    Radio() = default;
    Radio(const Radio &) = delete;
    Radio &operator=(const Radio &) = delete;

    // Trivial constructor, explicit Init (§10.14.1).
    //
    // **Bring-up is in two halves, and the reason is 41 KB of RAM.** This one
    // is the half that happens once and is never undone — NVS, `esp_netif`,
    // the default event loop and our own handlers on it. It costs about 10 KB,
    // it is paid the first time anything wants the radio, and it starts
    // nothing.
    //
    // The other half — the netifs and `esp_wifi_init` itself — is taken by the
    // first `StartClient` / `StartAp` / `Scan` and **given back by `Stop`**.
    // Measured on this board with `status`, at a steady state well after boot:
    // 170,524 bytes free having never touched the radio, 160,000-ish once it
    // has been used and switched off, and 119,256 with a client running. So
    // the releasable part is ~41 KB and the permanent part ~10 KB.
    //
    // 41 KB is worth this much machinery on a part with 512 KB of internal RAM
    // (§10.1) — and 26 on/off cycles
    // measured on the board return it every time, drifting a few hundred bytes
    // in *both* directions, which is the allocator rather than a leak.
    esp_err_t Init();
    bool Ready() const { return ready_; }

    // Whether the second half is currently up — i.e. whether the heap above is
    // currently spent. `Stop()` is what puts it back down.
    bool StackUp() const { return stack_ready_; }

    // Join `ssid`. Replaces whatever the station was doing. An empty password
    // means an open network.
    //
    // `ip` is per call rather than per device, because it is per *network*:
    // the same object joins a home that hands out addresses and an office that
    // does not. `nullptr`, or one with `enabled` false, is DHCP — and it
    // actively puts the DHCP client back if a previous network had switched it
    // off, which is the half of this that is easy to forget.
    esp_err_t StartClient(const char *ssid, const char *password,
                          const StaticIp *ip = nullptr);

    // Become an access point. An empty password means an open network — which
    // is the shipped default and is argued in `config.init.json`; a password
    // shorter than `kMinApPasswordLength` is refused rather than silently
    // dropped, because a wide-open AP nobody asked for is worse than an error.
    esp_err_t StartAp(const char *ssid, const char *password, uint8_t channel = 6);

    // Radio down, **and the Wi-Fi stack with it**: `esp_wifi_stop`,
    // `esp_wifi_deinit`, and the default netifs destroyed. Off means off, and
    // the heap comes back. Idempotent, and cheap to undo — bringing the stack
    // up again is tens of milliseconds, which is nothing next to the seconds
    // an association takes anyway.
    esp_err_t Stop();

    // A snapshot, taken under the status lock — every field belongs to the
    // same instant, which is the same promise `Axp2101::Read` makes about its
    // dozen registers for the same reason.
    Status Get() const;

    // What is on the air, sorted by signal. **Blocking**, for a second or two,
    // and it costs a connected station a beat because the radio has to leave
    // its channel (§10.8.6 says not to do it on a timer).
    //
    // **Works with the radio off**, and leaves it exactly as it found it: it
    // brings the station interface up — and the whole stack, if that was down
    // too — scans, and puts back down whatever it had to raise. Nothing is
    // broadcast: an access point, hidden or not, is more radio than a scan
    // needs, and `wifi_radio.cpp` says why. A scan on a device that lives with
    // Wi-Fi off therefore costs its heap only for the two seconds it runs.
    //
    // **2.4 GHz, because that is every band this chip has** — not a filter
    // this code applies. `wifi_radio.cpp` carries the compile-time assertion
    // that says so and fails the build on a part where it stops being true.
    //
    // Refused while another scan is running, rather than queued.
    esp_err_t Scan(ScanResult *out, size_t capacity, size_t *found);
    bool Scanning() const { return scanning_; }

    static const char *Name(Mode mode);
    static const char *Name(Link link);
    static const char *Name(Failure failure);

    // Exposed for the console's benefit and for tests: which of the three a
    // `wifi_err_reason_t` means.
    static Failure Classify(uint8_t reason);

   private:
    static void EventTrampoline(void *arg, const char *base, int32_t id, void *data);
    void OnEvent(const char *base, int32_t id, void *data);

    void SetLink(Link link, Failure failure, uint8_t reason);

    // The second half of bring-up: the default netifs and `esp_wifi_init`.
    // Idempotent, and taken by whichever of `StartClient` / `StartAp` / `Scan`
    // needs it first.
    esp_err_t EnsureStack();

    // Its opposite, and the thing that gives the heap back. Stops the station
    // if it is running, deinitialises, and **destroys the default netifs** —
    // creating those twice is not a thing that works, so they are made in
    // `EnsureStack` and unmade here rather than living forever.
    void ReleaseStack();

    // `esp_wifi_start`, and nothing about the stack.
    esp_err_t EnsureStarted();
    void StopStation();

    // Called once the station has associated: either the fixed address goes
    // on, or the DHCP client is put back in case the last network turned it
    // off. Both paths end in `IP_EVENT_STA_GOT_IP`, which is what the rest of
    // this driver treats as "connected".
    void ApplyAddressing();

    // `type` is an `esp_netif_dns_type_t`, taken as an int so this header
    // stays clear of `esp_netif.h` — the same reason the netif handles above
    // are `void *`.
    esp_err_t SetDns(uint32_t address, int type);

    // Written out rather than `++`: C++20 deprecates compound assignment on a
    // `volatile`, and the toolchain builds with `-Werror`. The load and the
    // store are two operations, which is safe here because only the manager
    // task ever tears an association down — the event task only consumes.
    void SuppressNextDisconnect() {
        suppress_disconnect_ = static_cast<uint8_t>(suppress_disconnect_ + 1);
    }

    bool ready_ = false;        // the once-ever half of Init
    bool stack_ready_ = false;  // esp_wifi_init has been paid for
    bool started_ = false;      // esp_wifi_start
    bool want_connect_ = false;
    volatile bool scanning_ = false;

    // **How many disconnection events to ignore.** Tearing down an attempt to
    // start another one produces a `STA_DISCONNECTED` that is ours, and
    // reporting it as a failure would fail the *new* attempt before it began.
    // Incremented before every deliberate disconnect and consumed by the
    // handler.
    volatile uint8_t suppress_disconnect_ = 0;

    // **A copy, not the caller's pointer.** The house firmware of §10.14.4
    // keeps a `IPConfig*` into its config object and applies it from an event
    // handler; that works there and would be a lifetime trap here, where the
    // console can edit the network list between the association starting and
    // the event arriving.
    StaticIp static_ip_ = {};

    Status status_ = {};

    // Short critical sections only, and never held across an `esp_wifi_*`
    // call: the event handler takes it too, and `esp_wifi_scan_start` with
    // `block=true` waits for a scan-done that the same event task delivers.
    // A lock held across that call is a deadlock rather than a slow poll.
    SemaphoreHandle_t lock_ = nullptr;
    StaticSemaphore_t lock_storage_ = {};

    void *sta_netif_ = nullptr;
    void *ap_netif_ = nullptr;
};

}  // namespace wifi
