#include "wifi_view.h"

namespace ui {
namespace {

// A bounded copy with a terminator, because 802.11 puts 32 bytes in an SSID and
// says nothing about a terminator — a scan record can arrive full. There is no
// `<cstring>` here on purpose: this file includes what its header includes and
// nothing more (§10.11), and a byte loop is smaller than the argument for
// relaxing that.
void Copy(char *out, size_t size, const char *in) {
    size_t i = 0;
    for (; in != nullptr && i + 1 < size && in[i] != '\0'; ++i) {
        out[i] = in[i];
    }
    out[i] = '\0';
}

bool Same(const char *a, const char *b) {
    size_t i = 0;
    for (; a[i] != '\0' && b[i] != '\0'; ++i) {
        if (a[i] != b[i]) {
            return false;
        }
    }
    return a[i] == b[i];
}

}  // namespace

WifiMode WifiModeFrom(bool active, bool ap) {
    if (!active) {
        return WifiMode::kOff;
    }
    return ap ? WifiMode::kAp : WifiMode::kClient;
}

bool WifiModeActive(WifiMode mode) { return mode != WifiMode::kOff; }

bool WifiModeIsAp(WifiMode mode) { return mode == WifiMode::kAp; }

WifiMode NextWifiMode(WifiMode mode) {
    switch (mode) {
        case WifiMode::kOff:
            return WifiMode::kClient;
        case WifiMode::kClient:
            return WifiMode::kAp;
        case WifiMode::kAp:
            break;
    }
    return WifiMode::kOff;
}

const char *WifiModeName(WifiMode mode) {
    switch (mode) {
        case WifiMode::kOff:
            return "off";
        case WifiMode::kClient:
            return "client";
        case WifiMode::kAp:
            return "ap";
    }
    return "off";
}

void WifiView::Opened() {
    selected_ = 0;
    index_ = 0;
    ScanClosed();
    noted_ = false;
    noted_at_ms_ = 0;
}

void WifiView::SetRecords(bool active, bool ap, uint8_t network_count) {
    mode_ = WifiModeFrom(active, ap);
    ap_ = ap;
    network_count_ = network_count;
    // **The access point is one record and a client is however many are
    // remembered**, including none — which is a state to draw rather than an
    // error, because it is what a device fresh from the flasher looks like.
    count_ = ap_ ? uint8_t{1} : network_count_;
    if (index_ >= count_) {
        // A network forgotten from the console while this screen is up. Clamped
        // rather than reset: the operator was reading a list that got shorter,
        // and the nearest end of it is the least surprising place to be.
        index_ = count_ > 0 ? static_cast<uint8_t>(count_ - 1) : uint8_t{0};
    }
}

void WifiView::Next() { selected_ = static_cast<uint8_t>((selected_ + 1) % kRowCount); }

void WifiView::Select(uint8_t row) {
    if (row >= kRowCount) {
        return;
    }
    selected_ = row;
}

WifiAction WifiView::Activate() {
    switch (Selected()) {
        case WifiRow::kMode: {
            const WifiMode next = NextWifiMode(mode_);
            mode_ = next;
            // **The record kind follows the press in the same pass**, so the
            // screen never draws a network under a row that says `ap`. Off says
            // nothing about which record to show, so it leaves that alone — and
            // the caller writes exactly these two facts back into the file.
            if (next != WifiMode::kOff) {
                const bool ap = WifiModeIsAp(next);
                if (ap != ap_) {
                    ap_ = ap;
                    index_ = 0;
                    count_ = ap_ ? uint8_t{1} : network_count_;
                }
            }
            return WifiAction::kModeChanged;
        }

        case WifiRow::kRecord:
            return StepNext();

        case WifiRow::kScan:
            // **Nowhere to put a name is its own answer.** The list is only
            // worth opening if something can hold what comes out of it, and this
            // device cannot be told to remember a new network from a screen
            // (§10.8.6) — so a client with no networks is told, rather than
            // shown a list that ends in nothing.
            return count_ > 0 ? WifiAction::kOpenScan : WifiAction::kNoRecord;

        case WifiRow::kCount:
            break;
    }
    return WifiAction::kNone;
}

WifiAction WifiView::StepNext() {
    if (!CanStep()) {
        return WifiAction::kNothingToStep;
    }
    index_ = static_cast<uint8_t>((index_ + 1) % count_);
    return WifiAction::kStepped;
}

WifiAction WifiView::StepPrev() {
    if (!CanStep()) {
        return WifiAction::kNothingToStep;
    }
    index_ = static_cast<uint8_t>((index_ + count_ - 1) % count_);
    return WifiAction::kStepped;
}

void WifiView::ScanOpened() {
    scan_state_ = WifiScanState::kScanning;
    scan_count_ = 0;
    scan_selected_ = 0;
    scan_window_ = 0;
}

void WifiView::ScanClosed() {
    scan_state_ = WifiScanState::kIdle;
    scan_count_ = 0;
    scan_selected_ = 0;
    scan_window_ = 0;
}

void WifiView::ScanFound(const WifiScanEntry *entries, uint8_t count) {
    if (scan_state_ != WifiScanState::kScanning) {
        return;
    }

    scan_count_ = 0;
    for (uint8_t i = 0; i < count && scan_count_ < kWifiScanMax; ++i) {
        if (entries == nullptr) {
            break;
        }
        WifiScanEntry candidate = {};
        Copy(candidate.ssid, sizeof candidate.ssid, entries[i].ssid);
        candidate.rssi = entries[i].rssi;
        candidate.secured = entries[i].secured;

        // A hidden access point scans as an empty name, and a name is the only
        // thing this list is for.
        if (candidate.ssid[0] == '\0') {
            continue;
        }
        // Two access points of one network are one network to somebody choosing
        // it, and the driver sorts by signal — so the copy that is kept is the
        // one that was strongest.
        bool already = false;
        for (uint8_t j = 0; j < scan_count_; ++j) {
            if (Same(scan_[j].ssid, candidate.ssid)) {
                already = true;
                break;
            }
        }
        if (already) {
            continue;
        }

        scan_[scan_count_] = candidate;
        ++scan_count_;
    }

    scan_selected_ = 0;
    scan_window_ = 0;
    scan_state_ = scan_count_ > 0 ? WifiScanState::kList : WifiScanState::kEmpty;
}

void WifiView::ScanFailed() {
    if (scan_state_ != WifiScanState::kScanning) {
        return;
    }
    scan_count_ = 0;
    scan_selected_ = 0;
    scan_window_ = 0;
    scan_state_ = WifiScanState::kFailed;
}

const WifiScanEntry *WifiView::ScanEntry(uint8_t index) const {
    if (index >= scan_count_) {
        return nullptr;
    }
    return &scan_[index];
}

void WifiView::ScanNext() {
    if (scan_count_ == 0) {
        return;
    }
    scan_selected_ = static_cast<uint8_t>((scan_selected_ + 1) % scan_count_);
    FollowSelection();
}

bool WifiView::ScanSelectRow(uint8_t visible_row) {
    if (visible_row >= kWifiScanRows) {
        return false;
    }
    const uint8_t absolute = static_cast<uint8_t>(scan_window_ + visible_row);
    if (absolute >= scan_count_) {
        // A tap on a row with no network on it. Ignored rather than clamped, the
        // same call `Select` makes: it landed on nothing.
        return false;
    }
    scan_selected_ = absolute;
    return true;
}

const char *WifiView::PickSelected(uint32_t now_ms) {
    if (scan_state_ != WifiScanState::kList || scan_count_ == 0) {
        return nullptr;
    }
    Noted(now_ms);
    return scan_[scan_selected_].ssid;
}

void WifiView::Noted(uint32_t now_ms) {
    noted_ = true;
    noted_at_ms_ = now_ms;
}

bool WifiView::Note(uint32_t now_ms) const {
    // Subtraction rather than a comparison of two absolute counters, so the
    // ~49-day wrap is arithmetic and not a case — every other window in this
    // firmware is written the same way.
    return noted_ && (now_ms - noted_at_ms_) < kNoteMs;
}

void WifiView::FollowSelection() {
    // The window follows the selection rather than the other way round, because
    // the selection is what a button moves — and a wrap to the top has to bring
    // the window with it, or the press that goes round leaves the operator
    // looking at rows with nothing selected in them.
    if (scan_selected_ < scan_window_) {
        scan_window_ = scan_selected_;
        return;
    }
    if (scan_selected_ >= scan_window_ + kWifiScanRows) {
        scan_window_ = static_cast<uint8_t>(scan_selected_ - kWifiScanRows + 1);
    }
}

}  // namespace ui
