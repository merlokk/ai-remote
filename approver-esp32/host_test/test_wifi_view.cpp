// The Wi-Fi screen (CLAUDE.md §10.8.6), tested where it costs nothing to test
// it (§10.11, host tier).
//
// `ui/wifi_view.h` includes `<cstdint>` and `<cstddef>` and nothing else, so
// this suite needs no fake — the navigator's shape rather than the drivers'.
//
// Where the value is, in the order the file is written:
//
//   * **the mode is two config fields and three states**, and the mapping
//     between them is the one thing on this screen that could quietly turn a
//     radio on. Every combination is a case here;
//   * **a record that cannot step and a record that does not exist are
//     different refusals**, because the second one is also the reason the scan
//     row has nothing to do — the owner's "you cannot add a network" arriving as
//     behaviour;
//   * **the list drops what nobody could act on**: a hidden network with no
//     name, the same name twice, a result that came back after the operator
//     left. A row that cannot be picked usefully is worse than a short list.

#include "unity.h"
#include "wifi_view.h"

#include <cstdio>
#include <cstring>

using ui::WifiAction;
using ui::WifiMode;
using ui::WifiRow;
using ui::WifiScanEntry;
using ui::WifiScanState;
using ui::WifiView;

namespace {

constexpr uint8_t kMode = static_cast<uint8_t>(WifiRow::kMode);
constexpr uint8_t kRecord = static_cast<uint8_t>(WifiRow::kRecord);
constexpr uint8_t kScan = static_cast<uint8_t>(WifiRow::kScan);

// A client with `count` remembered networks, freshly opened — the ordinary state
// of this screen, and enough of a fixture that no test has to spell it.
void Client(WifiView *view, uint8_t count) {
    view->Opened();
    view->SetRecords(true, false, count);
}

void Ap(WifiView *view, uint8_t networks) {
    view->Opened();
    view->SetRecords(true, true, networks);
}

// One scanned network. The strongest first, the way the driver hands them over.
WifiScanEntry Air(const char *ssid, int8_t rssi = -50, bool secured = true) {
    WifiScanEntry entry = {};
    std::snprintf(entry.ssid, sizeof entry.ssid, "%s", ssid);
    entry.rssi = rssi;
    entry.secured = secured;
    return entry;
}

// A list on the glass, ready to be stepped through.
void Listed(WifiView *view, const WifiScanEntry *entries, uint8_t count) {
    view->ScanOpened();
    view->ScanFound(entries, count);
}

}  // namespace

// --- The mode row --------------------------------------------------------

void test_the_mode_is_the_two_config_fields_together(void) {
    // `wifi.active` false is off whatever `wifi.mode` says, which is the rule
    // `config::Wifi` is written around: two fields that can disagree is one bug
    // report nobody can read.
    TEST_ASSERT_EQUAL_INT(static_cast<int>(WifiMode::kOff),
                          static_cast<int>(ui::WifiModeFrom(false, false)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(WifiMode::kOff),
                          static_cast<int>(ui::WifiModeFrom(false, true)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(WifiMode::kClient),
                          static_cast<int>(ui::WifiModeFrom(true, false)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(WifiMode::kAp),
                          static_cast<int>(ui::WifiModeFrom(true, true)));
}

void test_off_is_the_only_mode_that_is_not_active(void) {
    // The other direction, which is what the caller writes back into the file —
    // and getting it wrong is a device that comes up with its radio on because
    // somebody looked at a screen.
    TEST_ASSERT_FALSE(ui::WifiModeActive(WifiMode::kOff));
    TEST_ASSERT_TRUE(ui::WifiModeActive(WifiMode::kClient));
    TEST_ASSERT_TRUE(ui::WifiModeActive(WifiMode::kAp));

    TEST_ASSERT_TRUE(ui::WifiModeIsAp(WifiMode::kAp));
    TEST_ASSERT_FALSE(ui::WifiModeIsAp(WifiMode::kClient));
}

void test_the_mode_cycles_through_all_three_and_round(void) {
    // One button has to reach every state, which is what makes this screen
    // usable when the glass is the thing being tested.
    TEST_ASSERT_EQUAL_INT(static_cast<int>(WifiMode::kClient),
                          static_cast<int>(ui::NextWifiMode(WifiMode::kOff)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(WifiMode::kAp),
                          static_cast<int>(ui::NextWifiMode(WifiMode::kClient)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(WifiMode::kOff),
                          static_cast<int>(ui::NextWifiMode(WifiMode::kAp)));
}

void test_every_mode_has_a_word(void) {
    // A mode with no name is a row that draws nothing, which reads as a screen
    // that has stopped answering.
    const WifiMode every[] = {WifiMode::kOff, WifiMode::kClient, WifiMode::kAp};
    for (WifiMode mode : every) {
        TEST_ASSERT_NOT_NULL(ui::WifiModeName(mode));
        TEST_ASSERT_TRUE(ui::WifiModeName(mode)[0] != '\0');
    }
}

void test_pressing_the_mode_row_changes_the_mode(void) {
    WifiView view;
    Client(&view, 2);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(WifiMode::kClient), static_cast<int>(view.Mode()));

    TEST_ASSERT_EQUAL_INT(static_cast<int>(WifiAction::kModeChanged),
                          static_cast<int>(view.Activate()));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(WifiMode::kAp), static_cast<int>(view.Mode()));
}

void test_switching_to_the_access_point_shows_the_one_record_it_has(void) {
    // **The record kind follows the press immediately**, in the same pass — the
    // caller writes the two config fields and hands them back next time round,
    // and a screen that showed a network for one frame after being told it was
    // an access point is a screen that flickers on the one press that changes
    // what is on it.
    WifiView view;
    Client(&view, 3);
    view.StepNext();
    view.StepNext();
    TEST_ASSERT_EQUAL_UINT8(2, view.Index());

    view.Select(kMode);
    view.Activate();
    TEST_ASSERT_TRUE(view.ShowingAp());
    TEST_ASSERT_EQUAL_UINT8(1, view.Count());
    TEST_ASSERT_EQUAL_UINT8(0, view.Index());
}

void test_switching_off_leaves_the_record_it_was_showing(void) {
    // Off is a statement about the radio, not about which record the operator
    // was reading — so the access point's name stays on the glass and the mode
    // row is the only thing that changed.
    WifiView view;
    Ap(&view, 2);
    view.Select(kMode);
    view.Activate();  // ap → off

    TEST_ASSERT_EQUAL_INT(static_cast<int>(WifiMode::kOff), static_cast<int>(view.Mode()));
    TEST_ASSERT_TRUE(view.ShowingAp());
    TEST_ASSERT_EQUAL_UINT8(1, view.Count());
}

void test_the_console_switching_the_mode_reaches_the_screen(void) {
    // The screen mirrors the file rather than owning it, so `wifi mode ap` typed
    // over USB while somebody is looking at this screen changes what they see.
    WifiView view;
    Client(&view, 2);
    view.SetRecords(true, true, 2);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(WifiMode::kAp), static_cast<int>(view.Mode()));
    TEST_ASSERT_TRUE(view.ShowingAp());
    TEST_ASSERT_EQUAL_UINT8(1, view.Count());
}

// --- The rows ------------------------------------------------------------

void test_the_screen_opens_on_the_mode_row_at_the_first_record(void) {
    WifiView view;
    Client(&view, 3);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(WifiRow::kMode), static_cast<int>(view.Selected()));
    TEST_ASSERT_EQUAL_UINT8(0, view.Index());
}

void test_opening_the_screen_again_starts_at_the_first_record(void) {
    // A fresh visit is a fresh visit: coming into this screen from settings and
    // finding the third network under a finger is arriving somewhere nobody
    // asked for — and it is exactly what must *not* happen on the way back from
    // the scan list, which is the pair of rules that makes `Opened` its own call.
    WifiView view;
    Client(&view, 3);
    view.StepNext();
    view.StepNext();
    view.Next();
    Client(&view, 3);
    TEST_ASSERT_EQUAL_UINT8(0, view.Index());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(WifiRow::kMode), static_cast<int>(view.Selected()));
}

void test_the_rows_walk_and_wrap(void) {
    WifiView view;
    Client(&view, 1);
    view.Next();
    TEST_ASSERT_EQUAL_INT(static_cast<int>(WifiRow::kRecord), static_cast<int>(view.Selected()));
    view.Next();
    TEST_ASSERT_EQUAL_INT(static_cast<int>(WifiRow::kScan), static_cast<int>(view.Selected()));
    view.Next();
    TEST_ASSERT_EQUAL_INT(static_cast<int>(WifiRow::kMode), static_cast<int>(view.Selected()));
}

void test_the_walk_does_not_skip_a_row_that_can_do_nothing(void) {
    // A client with no networks has nothing to step and nowhere to put a name,
    // and both rows are still walked past: a selection that jumps over what it
    // is pointing at is a selection nobody can predict, and the row itself is
    // what explains why pressing it does nothing.
    WifiView view;
    Client(&view, 0);
    view.Next();
    TEST_ASSERT_EQUAL_INT(static_cast<int>(WifiRow::kRecord), static_cast<int>(view.Selected()));
    view.Next();
    TEST_ASSERT_EQUAL_INT(static_cast<int>(WifiRow::kScan), static_cast<int>(view.Selected()));
}

void test_a_tap_past_the_last_row_selects_nothing(void) {
    WifiView view;
    Client(&view, 2);
    view.Select(kScan);
    view.Select(WifiView::kRowCount);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(WifiRow::kScan), static_cast<int>(view.Selected()));
}

// --- Stepping the records ------------------------------------------------

void test_the_records_step_forward_and_wrap(void) {
    // Wrapping is what makes one button enough — `KEY` on that row is the whole
    // stepper when nobody is touching the arrows.
    WifiView view;
    Client(&view, 3);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(WifiAction::kStepped),
                          static_cast<int>(view.StepNext()));
    TEST_ASSERT_EQUAL_UINT8(1, view.Index());
    view.StepNext();
    TEST_ASSERT_EQUAL_UINT8(2, view.Index());
    view.StepNext();
    TEST_ASSERT_EQUAL_UINT8(0, view.Index());
}

void test_the_arrows_go_both_ways(void) {
    WifiView view;
    Client(&view, 3);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(WifiAction::kStepped),
                          static_cast<int>(view.StepPrev()));
    TEST_ASSERT_EQUAL_UINT8(2, view.Index());
    view.StepPrev();
    TEST_ASSERT_EQUAL_UINT8(1, view.Index());
}

void test_pressing_the_record_row_steps_it(void) {
    WifiView view;
    Client(&view, 2);
    view.Select(kRecord);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(WifiAction::kStepped),
                          static_cast<int>(view.Activate()));
    TEST_ASSERT_EQUAL_UINT8(1, view.Index());
}

void test_one_record_is_nothing_to_step_through(void) {
    WifiView view;
    Client(&view, 1);
    TEST_ASSERT_FALSE(view.CanStep());
    view.Select(kRecord);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(WifiAction::kNothingToStep),
                          static_cast<int>(view.Activate()));
    TEST_ASSERT_EQUAL_UINT8(0, view.Index());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(WifiAction::kNothingToStep),
                          static_cast<int>(view.StepPrev()));
    TEST_ASSERT_EQUAL_UINT8(0, view.Index());
}

void test_no_records_is_nothing_to_step_through_either(void) {
    WifiView view;
    Client(&view, 0);
    TEST_ASSERT_EQUAL_UINT8(0, view.Count());
    view.Select(kRecord);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(WifiAction::kNothingToStep),
                          static_cast<int>(view.Activate()));
    TEST_ASSERT_EQUAL_UINT8(0, view.Index());
}

void test_the_access_point_is_always_exactly_one_record(void) {
    WifiView view;
    Ap(&view, 4);
    TEST_ASSERT_EQUAL_UINT8(1, view.Count());
    TEST_ASSERT_FALSE(view.CanStep());
}

void test_a_network_forgotten_from_the_console_pulls_the_index_back(void) {
    // The console can edit the list while this screen is up, and an index left
    // pointing past the end is a screen reading a record that is not there —
    // which on this device means an SSID out of the middle of another struct.
    WifiView view;
    Client(&view, 3);
    view.StepNext();
    view.StepNext();
    TEST_ASSERT_EQUAL_UINT8(2, view.Index());

    view.SetRecords(true, false, 2);
    TEST_ASSERT_EQUAL_UINT8(1, view.Index());

    view.SetRecords(true, false, 0);
    TEST_ASSERT_EQUAL_UINT8(0, view.Index());
}

// --- The scan row --------------------------------------------------------

void test_the_scan_row_opens_the_list(void) {
    WifiView view;
    Client(&view, 1);
    view.Select(kScan);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(WifiAction::kOpenScan),
                          static_cast<int>(view.Activate()));
}

void test_with_no_record_there_is_nowhere_to_put_a_name(void) {
    // The owner's "you cannot add a network", arriving as behaviour: a name
    // picked off the air has to land in a record that exists, and a device with
    // none says so instead of growing the list.
    WifiView view;
    Client(&view, 0);
    view.Select(kScan);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(WifiAction::kNoRecord),
                          static_cast<int>(view.Activate()));
}

void test_the_access_point_always_has_a_record_to_fill(void) {
    // Even on a device that remembers no networks at all: the access point's own
    // name is a record, and naming it after something on the air is exactly what
    // the owner asked the list for.
    WifiView view;
    Ap(&view, 0);
    view.Select(kScan);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(WifiAction::kOpenScan),
                          static_cast<int>(view.Activate()));
}

// --- The list ------------------------------------------------------------

void test_opening_the_list_is_a_scan_in_flight(void) {
    WifiView view;
    view.ScanOpened();
    TEST_ASSERT_EQUAL_INT(static_cast<int>(WifiScanState::kScanning),
                          static_cast<int>(view.ScanState()));
    TEST_ASSERT_EQUAL_UINT8(0, view.ScanCount());
    TEST_ASSERT_NULL(view.PickSelected(1000));
}

void test_nothing_on_the_air_is_not_the_same_as_a_refused_scan(void) {
    // §10.9's rule about `unknown` being an honest state, on a screen: an empty
    // flat and a radio that would not look are different problems, and a screen
    // that spelled them the same way sends somebody to reboot a router that is
    // working.
    WifiView empty;
    empty.ScanOpened();
    empty.ScanFound(nullptr, 0);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(WifiScanState::kEmpty),
                          static_cast<int>(empty.ScanState()));

    WifiView failed;
    failed.ScanOpened();
    failed.ScanFailed();
    TEST_ASSERT_EQUAL_INT(static_cast<int>(WifiScanState::kFailed),
                          static_cast<int>(failed.ScanState()));
    TEST_ASSERT_NULL(failed.PickSelected(1000));
}

void test_the_list_arrives_selected_at_the_top(void) {
    WifiView view;
    const WifiScanEntry air[] = {Air("one"), Air("two"), Air("three")};
    Listed(&view, air, 3);

    TEST_ASSERT_EQUAL_INT(static_cast<int>(WifiScanState::kList),
                          static_cast<int>(view.ScanState()));
    TEST_ASSERT_EQUAL_UINT8(3, view.ScanCount());
    TEST_ASSERT_EQUAL_UINT8(0, view.ScanSelected());
    TEST_ASSERT_EQUAL_UINT8(0, view.ScanWindow());
    TEST_ASSERT_NOT_NULL(view.ScanEntry(0));
    TEST_ASSERT_EQUAL_STRING("one", view.ScanEntry(0)->ssid);
    TEST_ASSERT_NULL(view.ScanEntry(3));
}

void test_a_nameless_network_is_not_a_row(void) {
    // A hidden access point scans as an empty SSID. Picking one would write an
    // empty name into a record, which is a network nothing can ever join and a
    // row nobody can tell apart from the next one.
    WifiView view;
    const WifiScanEntry air[] = {Air(""), Air("real"), Air("")};
    Listed(&view, air, 3);

    TEST_ASSERT_EQUAL_UINT8(1, view.ScanCount());
    TEST_ASSERT_EQUAL_STRING("real", view.ScanEntry(0)->ssid);
}

void test_the_same_name_twice_is_one_row(void) {
    // Two access points of one network — a mesh, or a repeater — are one network
    // to somebody choosing a name, and the driver hands them over strongest
    // first, so the one that is kept is the one worth keeping.
    WifiView view;
    const WifiScanEntry air[] = {Air("home", -40), Air("guest", -50), Air("home", -70)};
    Listed(&view, air, 3);

    TEST_ASSERT_EQUAL_UINT8(2, view.ScanCount());
    TEST_ASSERT_EQUAL_STRING("home", view.ScanEntry(0)->ssid);
    TEST_ASSERT_EQUAL_INT8(-40, view.ScanEntry(0)->rssi);
    TEST_ASSERT_EQUAL_STRING("guest", view.ScanEntry(1)->ssid);
}

void test_a_crowded_flat_is_bounded(void) {
    WifiView view;
    WifiScanEntry air[ui::kWifiScanMax + 4] = {};
    for (uint8_t i = 0; i < ui::kWifiScanMax + 4; ++i) {
        char name[8] = {};
        std::snprintf(name, sizeof name, "n%u", static_cast<unsigned>(i));
        air[i] = Air(name);
    }
    Listed(&view, air, ui::kWifiScanMax + 4);

    TEST_ASSERT_EQUAL_UINT8(ui::kWifiScanMax, view.ScanCount());
}

void test_an_ssid_that_fills_the_field_survives_it(void) {
    // 802.11 puts 32 bytes in an SSID and says nothing about a terminator, so
    // the field can arrive full. A copy that trusted it would run off the end of
    // the row and print whatever is behind it.
    WifiView view;
    WifiScanEntry full = {};
    std::memset(full.ssid, 'x', sizeof full.ssid);  // no terminator at all
    full.rssi = -30;
    Listed(&view, &full, 1);

    TEST_ASSERT_EQUAL_UINT8(1, view.ScanCount());
    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(ui::kWifiSsidSize - 1),
                             static_cast<uint32_t>(std::strlen(view.ScanEntry(0)->ssid)));
}

void test_stepping_the_list_wraps_and_the_window_follows_it(void) {
    // Five rows fit and there are seven, so the selection has to be able to
    // reach the bottom two — and a wrap has to bring the window back to the top
    // with it, or the last press before the top leaves the operator looking at
    // rows nothing is selected in.
    WifiView view;
    WifiScanEntry air[7] = {};
    for (uint8_t i = 0; i < 7; ++i) {
        char name[8] = {};
        std::snprintf(name, sizeof name, "n%u", static_cast<unsigned>(i));
        air[i] = Air(name);
    }
    Listed(&view, air, 7);

    for (uint8_t i = 1; i < ui::kWifiScanRows; ++i) {
        view.ScanNext();
        TEST_ASSERT_EQUAL_UINT8(i, view.ScanSelected());
        TEST_ASSERT_EQUAL_UINT8(0, view.ScanWindow());  // still on the first page
    }

    view.ScanNext();  // one past the bottom of the window
    TEST_ASSERT_EQUAL_UINT8(ui::kWifiScanRows, view.ScanSelected());
    TEST_ASSERT_EQUAL_UINT8(1, view.ScanWindow());
    TEST_ASSERT_TRUE(view.ScanSelected() < view.ScanWindow() + ui::kWifiScanRows);

    view.ScanNext();  // the last one
    view.ScanNext();  // and round
    TEST_ASSERT_EQUAL_UINT8(0, view.ScanSelected());
    TEST_ASSERT_EQUAL_UINT8(0, view.ScanWindow());
}

void test_a_tap_on_a_visible_row_selects_the_one_under_the_finger(void) {
    WifiView view;
    WifiScanEntry air[7] = {};
    for (uint8_t i = 0; i < 7; ++i) {
        char name[8] = {};
        std::snprintf(name, sizeof name, "n%u", static_cast<unsigned>(i));
        air[i] = Air(name);
    }
    Listed(&view, air, 7);

    TEST_ASSERT_TRUE(view.ScanSelectRow(2));
    TEST_ASSERT_EQUAL_UINT8(2, view.ScanSelected());

    // …and it is the row's place on the glass, not its place in the list: three
    // more steps take the selection to the sixth network, which pushes the
    // window down by one — and the third row down is then the fourth network.
    view.ScanNext();
    view.ScanNext();
    view.ScanNext();
    TEST_ASSERT_EQUAL_UINT8(5, view.ScanSelected());
    TEST_ASSERT_EQUAL_UINT8(1, view.ScanWindow());
    view.ScanSelectRow(2);
    TEST_ASSERT_EQUAL_UINT8(3, view.ScanSelected());
}

void test_a_tap_past_the_end_of_the_list_selects_nothing(void) {
    WifiView view;
    const WifiScanEntry air[] = {Air("one"), Air("two")};
    Listed(&view, air, 2);

    TEST_ASSERT_TRUE(view.ScanSelectRow(1));
    // **And it says so**, because a tap that selected nothing must not pick the
    // network that was selected before it.
    TEST_ASSERT_FALSE(view.ScanSelectRow(4));  // a visible row with no network on it
    TEST_ASSERT_EQUAL_UINT8(1, view.ScanSelected());
    TEST_ASSERT_FALSE(view.ScanSelectRow(ui::kWifiScanRows));  // and one that is off screen
    TEST_ASSERT_EQUAL_UINT8(1, view.ScanSelected());
}

void test_picking_hands_back_the_name_that_is_selected(void) {
    WifiView view;
    const WifiScanEntry air[] = {Air("one"), Air("two"), Air("three")};
    Listed(&view, air, 3);
    view.ScanNext();

    const char *picked = view.PickSelected(5000);
    TEST_ASSERT_NOT_NULL(picked);
    TEST_ASSERT_EQUAL_STRING("two", picked);
}

void test_picking_leaves_the_record_it_is_for_where_it_was(void) {
    // The whole reason coming back from the list is not `Opened()`: a name
    // picked while looking at network 3 has to land in network 3.
    WifiView view;
    Client(&view, 3);
    view.StepNext();
    view.StepNext();

    const WifiScanEntry air[] = {Air("one")};
    Listed(&view, air, 1);
    TEST_ASSERT_NOT_NULL(view.PickSelected(1000));
    TEST_ASSERT_EQUAL_UINT8(2, view.Index());
}

void test_a_scan_nobody_is_waiting_for_is_ignored(void) {
    // The scan runs on a task of its own and takes a second or two, so it can
    // come back after the operator has left the screen — the rule
    // `link_policy.h` and `sync_policy.h` both keep about a result nobody asked
    // for, arriving here as a list appearing over a screen that is no longer up.
    WifiView view;
    view.ScanOpened();
    view.ScanClosed();

    const WifiScanEntry air[] = {Air("late")};
    view.ScanFound(air, 1);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(WifiScanState::kIdle),
                          static_cast<int>(view.ScanState()));
    TEST_ASSERT_EQUAL_UINT8(0, view.ScanCount());

    view.ScanFailed();
    TEST_ASSERT_EQUAL_INT(static_cast<int>(WifiScanState::kIdle),
                          static_cast<int>(view.ScanState()));
}

void test_leaving_the_list_drops_it(void) {
    WifiView view;
    const WifiScanEntry air[] = {Air("one"), Air("two")};
    Listed(&view, air, 2);
    view.ScanClosed();

    TEST_ASSERT_EQUAL_INT(static_cast<int>(WifiScanState::kIdle),
                          static_cast<int>(view.ScanState()));
    TEST_ASSERT_EQUAL_UINT8(0, view.ScanCount());
    TEST_ASSERT_NULL(view.PickSelected(1000));
}

void test_opening_the_screen_drops_a_list_from_the_last_visit(void) {
    WifiView view;
    const WifiScanEntry air[] = {Air("one")};
    Listed(&view, air, 1);
    Client(&view, 2);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(WifiScanState::kIdle),
                          static_cast<int>(view.ScanState()));
}

// --- The line that says an edit is only in memory ------------------------

void test_picking_says_that_it_is_only_in_memory(void) {
    WifiView view;
    const WifiScanEntry air[] = {Air("one")};
    Listed(&view, air, 1);

    TEST_ASSERT_FALSE(view.Note(1000));
    view.PickSelected(1000);
    TEST_ASSERT_TRUE(view.Note(1000));
    TEST_ASSERT_TRUE(view.Note(1000 + WifiView::kNoteMs - 1));
    TEST_ASSERT_FALSE(view.Note(1000 + WifiView::kNoteMs));
}

void test_the_note_survives_the_millisecond_wrap(void) {
    // ~49 days, and for this screen it is not a hypothetical: reaching it means
    // a device that has been sitting on a desk being a clock. Written as a
    // subtraction rather than a comparison of two absolute counters, like every
    // other window in this firmware.
    WifiView view;
    const WifiScanEntry air[] = {Air("one")};
    Listed(&view, air, 1);

    const uint32_t before = 0xFFFFFF00u;
    view.PickSelected(before);
    TEST_ASSERT_TRUE(view.Note(before + 16));                     // not yet wrapped
    TEST_ASSERT_TRUE(view.Note(400));                             // wrapped, 656 ms after
    TEST_ASSERT_FALSE(view.Note(before + WifiView::kNoteMs));
}

void RegisterWifiViewTests(void) {
    RUN_TEST(test_the_mode_is_the_two_config_fields_together);
    RUN_TEST(test_off_is_the_only_mode_that_is_not_active);
    RUN_TEST(test_the_mode_cycles_through_all_three_and_round);
    RUN_TEST(test_every_mode_has_a_word);
    RUN_TEST(test_pressing_the_mode_row_changes_the_mode);
    RUN_TEST(test_switching_to_the_access_point_shows_the_one_record_it_has);
    RUN_TEST(test_switching_off_leaves_the_record_it_was_showing);
    RUN_TEST(test_the_console_switching_the_mode_reaches_the_screen);

    RUN_TEST(test_the_screen_opens_on_the_mode_row_at_the_first_record);
    RUN_TEST(test_opening_the_screen_again_starts_at_the_first_record);
    RUN_TEST(test_the_rows_walk_and_wrap);
    RUN_TEST(test_the_walk_does_not_skip_a_row_that_can_do_nothing);
    RUN_TEST(test_a_tap_past_the_last_row_selects_nothing);

    RUN_TEST(test_the_records_step_forward_and_wrap);
    RUN_TEST(test_the_arrows_go_both_ways);
    RUN_TEST(test_pressing_the_record_row_steps_it);
    RUN_TEST(test_one_record_is_nothing_to_step_through);
    RUN_TEST(test_no_records_is_nothing_to_step_through_either);
    RUN_TEST(test_the_access_point_is_always_exactly_one_record);
    RUN_TEST(test_a_network_forgotten_from_the_console_pulls_the_index_back);

    RUN_TEST(test_the_scan_row_opens_the_list);
    RUN_TEST(test_with_no_record_there_is_nowhere_to_put_a_name);
    RUN_TEST(test_the_access_point_always_has_a_record_to_fill);

    RUN_TEST(test_opening_the_list_is_a_scan_in_flight);
    RUN_TEST(test_nothing_on_the_air_is_not_the_same_as_a_refused_scan);
    RUN_TEST(test_the_list_arrives_selected_at_the_top);
    RUN_TEST(test_a_nameless_network_is_not_a_row);
    RUN_TEST(test_the_same_name_twice_is_one_row);
    RUN_TEST(test_a_crowded_flat_is_bounded);
    RUN_TEST(test_an_ssid_that_fills_the_field_survives_it);
    RUN_TEST(test_stepping_the_list_wraps_and_the_window_follows_it);
    RUN_TEST(test_a_tap_on_a_visible_row_selects_the_one_under_the_finger);
    RUN_TEST(test_a_tap_past_the_end_of_the_list_selects_nothing);
    RUN_TEST(test_picking_hands_back_the_name_that_is_selected);
    RUN_TEST(test_picking_leaves_the_record_it_is_for_where_it_was);
    RUN_TEST(test_a_scan_nobody_is_waiting_for_is_ignored);
    RUN_TEST(test_leaving_the_list_drops_it);
    RUN_TEST(test_opening_the_screen_drops_a_list_from_the_last_visit);

    RUN_TEST(test_picking_says_that_it_is_only_in_memory);
    RUN_TEST(test_the_note_survives_the_millisecond_wrap);
}
