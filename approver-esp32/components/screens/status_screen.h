#pragma once

// The status pages (CLAUDE.md §10.8.5), painted.
//
// **A readout and nothing else**, the way the limits screen is: nothing on it
// can be acted on, and the only touch it takes is "next page" and "back". What
// is on each page is decided by whoever fills a `StatusFacts` — this file turns
// twelve strings into twelve labels and has no opinions about any of them.
//
// The pages are `ui/status_pages.h`'s, which includes `<cstdint>` and nothing
// else and is where the wrapping is tested. Everything below is layout.
//
// **Why it is a page of text and not a dashboard.** Every number here is one an
// operator reads once while looking for something — why did it restart, is the
// battery charging, is the accelerometer alive. A gauge is for a quantity that
// is watched; a line of text is for a fact that is looked up, and the whole of
// this screen is the second kind.

#include <cstdint>

#include "esp_err.h"
#include "lvgl.h"
#include "status_pages.h"

namespace screens {

// --- The layout ----------------------------------------------------------
inline constexpr int32_t kStatusPad = 24;
inline constexpr int32_t kStatusTitleTop = 22;

inline constexpr int32_t kStatusFirstRowTop = 92;
inline constexpr int32_t kStatusRowStride = 42;

// Where the value column starts. The label is left of it and faint; the value is
// right of it and bright — which is the same hierarchy-by-colour §10.8.3 settled
// on rather than paying ninety-seven kilobytes of flash for a second font size.
//
// **A label has about eight characters of room**, measured on the glass rather
// than computed: `firmware` fits and `magnitude` did not. The column is clipped
// so that a ninth one is a cut-off word instead of two words drawn on top of
// each other, which is what it was before somebody looked.
inline constexpr int32_t kStatusValueLeft = 176;

// How many lines a page may have. Nine at this stride fills the glass with a
// margin at the bottom, and a page that wanted a tenth is a page that wanted to
// be two — which is what the pager is for.
inline constexpr uint8_t kStatusRows = 9;

// The longest value that fits between `kStatusValueLeft` and the right margin at
// Montserrat 28. Counted rather than guessed, and generous by one: a value that
// runs off the edge is a number read wrong.
inline constexpr size_t kStatusValueSize = 22;

static_assert(kStatusFirstRowTop + (kStatusRows - 1) * kStatusRowStride + 34 <= 480,
              "the status rows would run off the bottom of the glass");

// One page, filled in by the caller. Labels are literals — they never change —
// and the values are copied in, because the caller's are on a stack somewhere.
struct StatusFacts {
    const char *title = "";
    const char *label[kStatusRows] = {};
    char value[kStatusRows][kStatusValueSize] = {};
    uint8_t rows = 0;

    // **Which page these facts are of, carried with them rather than read live.**
    // The rows are gathered outside the LVGL lock — one of them is an I²C read —
    // and the page can turn between the gathering and the painting. A title taken
    // from the pager at paint time would then name a page whose numbers are not
    // on the glass yet, which is a readout lying for a tenth of a second about
    // the one thing it is for.
    uint8_t page = 0;
    uint8_t page_count = 0;
};

class StatusScreen {
   public:
    StatusScreen() = default;
    StatusScreen(const StatusScreen &) = delete;
    StatusScreen &operator=(const StatusScreen &) = delete;

    esp_err_t Create(lv_obj_t *parent);
    bool Ready() const { return root_ != nullptr; }

    void SetVisible(bool visible);

    // What to show. Idempotent — called ten times a second with the same facts it
    // repaints nothing — and under the caller's LVGL lock.
    void Apply(const StatusFacts &facts);

    // The same handoff the settings list uses, and for the same reason: an LVGL
    // callback records what a finger did and the screens task is what acts on it.
    bool TakeNext();
    bool TakeBack();

   private:
    static void BodyClicked(lv_event_t *event);
    static void BackClicked(lv_event_t *event);

    lv_obj_t *root_ = nullptr;
    lv_obj_t *title_ = nullptr;
    lv_obj_t *page_ = nullptr;
    lv_obj_t *body_ = nullptr;
    lv_obj_t *label_[kStatusRows] = {};
    lv_obj_t *value_[kStatusRows] = {};

    char title_text_[24] = {};
    char page_text_[12] = {};
    char value_text_[kStatusRows][kStatusValueSize] = {};

    bool next_ = false;
    bool back_ = false;
    bool visible_ = false;
};

}  // namespace screens
