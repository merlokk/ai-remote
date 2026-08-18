#pragma once

// Which page of the status readout is up (CLAUDE.md §10.8.5).
//
// `<cstdint>` and nothing else, like everything else in `components/ui`. It is
// small enough to be embarrassing and it is here rather than in the screen for
// the reason the whole layer exists: a pager that wraps is a rule, the screen is
// pixels, and §10.11 can only reach one of the two.
//
// **Pages rather than one scrolling wall**, and that is the repository owner's
// call ("может несколько экранов"). The alternative is a scrollable list, which
// on this device means a finger dragging over numbers that are being repainted
// underneath it — and a screen whose content moves while it is read is a screen
// nobody trusts. A page is a whole thought, and `BOOT` steps to the next one.

#include <cstdint>

namespace ui {

// Three, and each one answers a different question rather than being a third of
// an answer: what the power is doing, what the machine is doing, and what the
// board is physically doing.
enum class StatusPage : uint8_t {
    kPower = 0,  // the battery, the rails, and why the board is awake at all
    kSystem,     // why it last restarted, what it is running, and what it is on
    kMotion,     // the IMU — a diagnostic, and §10.13's rule that it decides nothing
    kCount,
};

class StatusPager {
   public:
    static constexpr uint8_t kPageCount = static_cast<uint8_t>(StatusPage::kCount);

    StatusPager() = default;
    StatusPager(const StatusPager &) = delete;
    StatusPager &operator=(const StatusPager &) = delete;

    StatusPage Page() const { return static_cast<StatusPage>(index_); }
    uint8_t Index() const { return index_; }

    // Wrapping, deliberately: three pages reached by one button, and a last page
    // that refuses to go anywhere is a button that looks broken on the page the
    // operator happens to be on.
    void Next() { index_ = static_cast<uint8_t>((index_ + 1) % kPageCount); }

    // The screen came up. Back to the first page — arriving on whichever page was
    // last read is arriving somewhere the operator did not ask for.
    void Reset() { index_ = 0; }

   private:
    uint8_t index_ = 0;
};

}  // namespace ui
