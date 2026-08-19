#pragma once

// The Wi-Fi screen, decided where it can be tested (CLAUDE.md §10.8.6, §10.11).
//
// **This file includes `<cstdint>` and `<cstddef>` and nothing else**, which by
// now is the shape of this firmware rather than a choice made per screen — the
// navigator, the clock face, the request card, the limits, the settings list,
// the panel's idle timer, the Wi-Fi policy, the internet check, the sync
// schedule and the bus link all manage it. What lives here is which row is
// selected, which record is on the glass, what a press on each row means, and
// what a list of scanned networks does; `screens/wifi_screen.cpp` and
// `screens/wifi_scan_screen.cpp` turn that into widgets and have no rules in
// them.
//
// **This is deliberately not the screen §10.8.6 specified.** That one is a scan
// list with a keyboard behind it; this is the reduced one the repository owner
// asked for, and the difference is what is *settable*:
//
//   * the **mode** — off, client or access point. Three states rather than the
//     two the request named, because the shipped `config.json` has
//     `wifi.active` false: a two-state row would silently switch the radio on
//     and offer no way to switch it back off, and `wifimgr::Desired` already
//     has exactly these three;
//   * **one record at a time** — the access point's own name and password in AP
//     mode, one remembered network's in client mode, stepped through with
//     arrows when there is more than one;
//   * **a name off the air** — the scan list, reached by a press, and picking
//     from it writes the name into the record that is on the glass.
//
// And what is deliberately *not*:
//
//   * **no keyboard**, so a password is read and never typed. §10.8.6 has that
//     screen in millimetres and it is a piece of work of its own; until it
//     exists, `wifi join` on the console is how a password gets onto a device;
//   * **no way to add a record.** The owner's instruction, and it is the reason
//     the scan row refuses with a sentence when there is no record to fill
//     rather than quietly growing the list;
//   * **nothing here writes a file.** A press changes what is in memory and
//     `config save` is what keeps it, which is the rule §10.15 states for every
//     other setter and the one the touch calibration next door already follows.
//
// One rule worth stating before the code, because it is the only one that is
// not obvious from the names: **a row that cannot do anything is still a row.**
// The selection does not skip it and pressing it answers with its own value, so
// the screen can say why — the same call `settings_menu.h` makes about the rows
// with nothing behind them, and §10.9's about `unknown` being an honest state.

#include <cstddef>
#include <cstdint>

namespace ui {

// --- The mode row --------------------------------------------------------
//
// `wifimgr::Desired`'s three values, spelled again here rather than included:
// this file includes nothing (see above), and the mapping onto the two fields
// `config.json` really holds — `wifi.active` and `wifi.mode` — is the pair of
// functions below rather than something the screen layer works out for itself.
enum class WifiMode : uint8_t {
    kOff = 0,  // the radio is down; the device is a clock and nothing else
    kClient,   // join one of the remembered networks
    kAp,       // be an access point
};

// The two config fields into one answer. **`active` false is off whatever the
// mode says** — one switch that means radio-down rather than two fields that
// can disagree, which is the rule `config::Wifi` is written around.
WifiMode WifiModeFrom(bool active, bool ap);

// …and back out again, for the caller that has to write those two fields.
bool WifiModeActive(WifiMode mode);
bool WifiModeIsAp(WifiMode mode);

// What a press on the mode row does. off → client → ap → off, so one button is
// enough to reach all three — which is what makes this screen usable when the
// glass is the thing being debugged.
WifiMode NextWifiMode(WifiMode mode);

const char *WifiModeName(WifiMode mode);

// --- The rows ------------------------------------------------------------
enum class WifiRow : uint8_t {
    kMode = 0,  // off / client / ap
    kRecord,    // which record is on the glass, and the arrows that step it
    kScan,      // → the list of what is on the air
    kCount,
};

// What a press produced. Each refusal is its own value because each is its own
// sentence on the glass: "there is only one of these" and "there is nowhere to
// put a name" send somebody in different directions.
enum class WifiAction : uint8_t {
    kNone = 0,
    kModeChanged,    // the mode row: `Mode()` is the new one, and the caller writes it
    kStepped,        // the record row: `Index()` is the record now shown
    kNothingToStep,  // …and the same row when there is one record or none
    kOpenScan,       // the scan row
    kNoRecord,       // …and the same row with no record for a name to land in
};

// --- The scan list -------------------------------------------------------

// Must match `wifi::kMaxScanResults`, and the two are tied by a `static_assert`
// where they meet — this file includes nothing, so it cannot say so itself.
inline constexpr uint8_t kWifiScanMax = 16;

// How many of them fit on the glass at once. Five rows of the same stride the
// settings list uses, which is what a 480-pixel panel holds under a header at
// Montserrat 28 — measured there rather than computed here.
inline constexpr uint8_t kWifiScanRows = 5;

// Same numbers as `config::kSsidSize` / `kPasswordSize`, and the same reason:
// 802.11 puts 32 bytes in an SSID and 63 characters in a WPA passphrase.
inline constexpr size_t kWifiSsidSize = 33;
inline constexpr size_t kWifiPasswordSize = 65;

// One network on the air. Ours rather than `wifi::ScanResult`, because that one
// arrives with `esp_err.h` and FreeRTOS behind it — the screen layer copies
// across, which costs a loop and keeps this file compilable by a bare host
// compiler (§10.11).
struct WifiScanEntry {
    char ssid[kWifiSsidSize];
    int8_t rssi;
    bool secured;
};

// **Four states, and two of them are the difference between "nothing is there"
// and "I could not look".** A scan needs the radio for a second or two and can
// be refused; a screen that spelled that the same way as an empty flat would be
// a screen that sends somebody hunting a router that is working.
enum class WifiScanState : uint8_t {
    kIdle = 0,  // no list, and none asked for
    kScanning,
    kList,
    kEmpty,   // nothing on the air
    kFailed,  // the radio would not do it
};

class WifiView {
   public:
    static constexpr uint8_t kRowCount = static_cast<uint8_t>(WifiRow::kCount);

    // How long the line saying an edit is in memory stays on the glass. Long
    // enough to read after a press, short enough that it is gone before the
    // operator wonders whether it is a warning — and the console is where a
    // fact that has to outlive a glance belongs (`config` prints it).
    static constexpr uint32_t kNoteMs = 8000;

    WifiView() = default;
    WifiView(const WifiView &) = delete;
    WifiView &operator=(const WifiView &) = delete;

    // --- The record half --------------------------------------------------

    // The screen came up *from settings*. Back at the first row and the first
    // record, and any list from a previous visit dropped.
    //
    // **Coming back from the scan list is not this**, and that is the point of
    // it being a separate call: a name picked for the record on the glass has to
    // land in the record on the glass, so the index must survive the trip.
    void Opened();

    // What `config.json` says, handed in on every pass — so the console
    // changing the mode or forgetting a network is visible here without this
    // layer ever having read a file (§10.14.2).
    //
    // **`ap` decides which record is shown and `active` does not**, which is
    // what makes the third mode possible at all: a device with the radio off
    // still has an access point's name and a list of networks, and the operator
    // has to be able to look at whichever of them the mode underneath names.
    void SetRecords(bool active, bool ap, uint8_t network_count);

    WifiMode Mode() const { return mode_; }
    bool ShowingAp() const { return ap_; }

    // How many records there are of the kind being shown: the access point is
    // always exactly one, a client is however many networks are remembered —
    // including none, which is a state this screen has to draw rather than an
    // error.
    uint8_t Count() const { return count_; }
    uint8_t Index() const { return index_; }
    bool CanStep() const { return count_ > 1; }

    WifiRow Selected() const { return static_cast<WifiRow>(selected_); }

    // The next row, wrapping. Nothing is skipped — see the head of this file.
    void Next();

    // Point at a row: what a tap does. Out of range is ignored rather than
    // clamped, the same call `settings_menu.h` makes — a tap that landed between
    // rows selected nothing, and moving to whichever end was nearest is a guess.
    void Select(uint8_t row);

    // Act on the selected row.
    WifiAction Activate();

    // The two arrows, which are the one thing on this screen a finger can reach
    // that is not a row. Both wrap, which is also what lets `Activate` on the
    // record row be a single-button substitute for them.
    WifiAction StepNext();
    WifiAction StepPrev();

    // --- The list half ----------------------------------------------------

    // The list screen came up: a scan is in flight, whatever the last one found.
    void ScanOpened();

    // …and it went away. The list is dropped rather than kept: a list reopened
    // an hour later would be a photograph of a flat somebody has since left.
    void ScanClosed();

    WifiScanState ScanState() const { return scan_state_; }

    // What a scan came back with. **Ignored unless one is in flight**, which is
    // the rule `link_policy.h` and `sync_policy.h` both keep about a result
    // nobody asked for — here it is a scan that finished after the operator left
    // the screen.
    //
    // Three things happen on the way in, and each is a rule rather than tidying:
    // a nameless network is dropped (a hidden AP scans as an empty SSID, and
    // picking one would set an empty name), a name already in the list is
    // dropped (two access points of one network are one network to somebody
    // choosing it), and anything past `kWifiScanMax` is ignored.
    void ScanFound(const WifiScanEntry *entries, uint8_t count);

    // The radio refused. Its own state, for the reason `WifiScanState` says.
    void ScanFailed();

    uint8_t ScanCount() const { return scan_count_; }
    const WifiScanEntry *ScanEntry(uint8_t index) const;

    uint8_t ScanSelected() const { return scan_selected_; }

    // The first row on the glass. The window follows the selection rather than
    // the other way round, because the selection is what a button moves.
    uint8_t ScanWindow() const { return scan_window_; }

    void ScanNext();

    // A tap on one of the `kWifiScanRows` visible rows. Past the end of the list
    // is ignored, like `Select` above — and **the answer is whether it landed on
    // a network**, because on this screen a tap both selects and picks: a press
    // on an empty row that returned nothing to say so would pick whatever was
    // selected before it, which is a network nobody aimed at.
    bool ScanSelectRow(uint8_t visible_row);

    // The name that was selected, or null when there is nothing to pick. The
    // caller is what writes it into a record and it is what knows where — this
    // layer has never seen a config field.
    //
    // It takes the clock because a pick starts the note below: an edit that is
    // only in memory has to say so, once, where the person who made it is
    // looking.
    const char *PickSelected(uint32_t now_ms);

    // Is the "in memory only" line up right now.
    bool Note(uint32_t now_ms) const;

    // Something else worth saying that line about — a mode changed by a press.
    // Same window, same reason.
    void Noted(uint32_t now_ms);

   private:
    void FollowSelection();

    // What the file says, mirrored here on every pass.
    WifiMode mode_ = WifiMode::kOff;
    bool ap_ = false;
    uint8_t network_count_ = 0;

    uint8_t count_ = 0;
    uint8_t index_ = 0;
    uint8_t selected_ = 0;

    WifiScanState scan_state_ = WifiScanState::kIdle;
    WifiScanEntry scan_[kWifiScanMax] = {};
    uint8_t scan_count_ = 0;
    uint8_t scan_selected_ = 0;
    uint8_t scan_window_ = 0;

    bool noted_ = false;
    uint32_t noted_at_ms_ = 0;
};

}  // namespace ui
