#pragma once

// The request card, painted (CLAUDE.md §10.8.4). Every decision it shows was
// taken in `ui/request_card.h`, which is host-tested; this file has no rules in
// it, only a layout and a palette.
//
// **It is an overlay, not a screen.** §10.8.1: the card appears over whatever is
// up and what was underneath comes back exactly — "structurally, not by restoring
// anything". So this is a full-screen object parented to the same LVGL screen the
// clock is on, hidden when there is nothing to ask and moved to the foreground
// when there is. The clock keeps drawing behind it and never knows.
//
// Three things about the decision surface, and the first two are
// [`approver-web`](../../../approver-web/CLAUDE.md)'s "Look and feel" constraint 1
// carried over — which §10.8.4 says matters *more* here, because a gadget invites
// reflex taps:
//
//   * **the two plates are the same size and the same weight.** Neither is the
//     quiet one you dismiss. They are told apart by the word on them, by the name
//     of the physical button that presses them, and by colour — in that order,
//     because colour is the one of the three a glance in a dark room gets wrong;
//   * **the `tool_input` outweighs both of them** and it **scrolls**. §10.8.4:
//     "a card whose command has not been fully seen is exactly the card people
//     approve by reflex". It is never shortened — a payload too big for the model
//     is refused before it ever reaches this file, and `request_card.h` says why;
//   * **nothing here is touchable.** The verdict comes from `BOOT` and `PWR`, and
//     the plates are labels naming them rather than buttons. That is stricter than
//     §10.8.4 asks for and it is deliberate: a stray finger on a 480×480 panel
//     cannot approve anything at all, and §10.8.1's queued-touch guard becomes a
//     guard on when a *press* began. The one thing touch still does is drag the
//     command into view, which decides nothing.

#include <cstdint>

#include "esp_err.h"
#include "lvgl.h"
#include "request_card.h"

namespace screens {

// --- The layout ----------------------------------------------------------
inline constexpr int32_t kCardPad = 20;

inline constexpr int32_t kCardHeaderTop = 14;
inline constexpr int32_t kCardToolTop = 52;
inline constexpr int32_t kCardCwdTop = 94;
inline constexpr int32_t kCardRuleTop = 128;

inline constexpr int32_t kCardInputTop = 140;
inline constexpr int32_t kCardInputHeight = 216;

inline constexpr int32_t kCardPlateTop = 372;
inline constexpr int32_t kCardPlateHeight = 88;
inline constexpr int32_t kCardPlateGap = 16;

class RequestScreen {
   public:
    RequestScreen() = default;
    RequestScreen(const RequestScreen &) = delete;
    RequestScreen &operator=(const RequestScreen &) = delete;

    // Builds the overlay under `parent`, hidden. **The caller holds the LVGL
    // lock** — the same contract `ClockScreen::Create` has, and for the same
    // reason: a class that took the lock itself is a class a caller can nest.
    esp_err_t Create(lv_obj_t *parent);
    bool Ready() const { return root_ != nullptr; }

    // What to show. Idempotent: called fifty times a second with the same card it
    // touches nothing. Also under the caller's lock.
    //
    // `note` is the line under a receipt for a card somebody **answered** — what
    // actually left the device. It is the caller's to write because this file
    // cannot know: until §10.6 exists there is no key, so nothing leaves, and
    // saying "sent" would be the one lie on this screen that matters (§10.10).
    //
    // A card that **timed out** does not get it: nobody decided anything, so a
    // note about a decision would be false in the other direction. That line is
    // this file's own and needs no knowledge of a signer.
    void Apply(const ui::RequestCard &card, uint32_t now_ms, const char *note);

   private:
    void ShowCard(const ui::Request &request, uint32_t remaining_ms, uint8_t waiting);
    void ShowReceipt(const ui::RequestCard &card, const char *note);
    void Hide();

    lv_obj_t *root_ = nullptr;

    // The card.
    lv_obj_t *card_ = nullptr;
    lv_obj_t *title_ = nullptr;
    lv_obj_t *countdown_ = nullptr;
    lv_obj_t *waiting_ = nullptr;
    lv_obj_t *tool_ = nullptr;
    lv_obj_t *cwd_ = nullptr;
    lv_obj_t *rule_ = nullptr;
    lv_obj_t *input_box_ = nullptr;
    lv_obj_t *input_ = nullptr;
    lv_obj_t *allow_plate_ = nullptr;
    lv_obj_t *deny_plate_ = nullptr;

    // The receipt.
    lv_obj_t *receipt_ = nullptr;
    lv_obj_t *outcome_ = nullptr;
    lv_obj_t *receipt_tool_ = nullptr;
    lv_obj_t *note_ = nullptr;

    // Every label points at one of these rather than at LVGL's own copy, so a
    // countdown that changes once a second costs no allocation out of a 64 KB
    // pool (§10.14.1).
    char countdown_text_[12] = {};
    char waiting_text_[20] = {};
    char tool_text_[ui::kToolNameSize] = {};
    char cwd_text_[ui::kCwdSize] = {};
    char input_text_[ui::kToolInputSize] = {};
    char outcome_text_[16] = {};
    char receipt_tool_text_[ui::kToolNameSize] = {};
    char note_text_[64] = {};

    // What is currently on the glass, so `Apply` can do nothing when nothing
    // changed. The countdown is kept in whole seconds for the same reason: it is
    // the only field that would otherwise repaint at the tick rate.
    ui::CardState shown_ = ui::CardState::kIdle;
    uint32_t shown_seconds_ = 0xFFFFFFFFu;

    // **How many were waiting, tracked apart from which card is up**, because the
    // two change independently: a second request arriving does not replace the
    // card, and the first version updated "+N waiting" only when the card itself
    // changed — so a queue that grew under a card said nothing. Found by looking
    // at a screenshot next to `request`, which is the pair §10.12.2 exists for.
    uint8_t shown_waiting_ = 0xFF;

    // **Which card is up, identified by its nonce and not by its address.** The
    // queue shifts when one is answered, so the next card lands in the slot the
    // last one occupied and a pointer comparison would say nothing changed — a
    // second request quietly wearing the first one's command. §7 gives every
    // request 32 random bytes precisely so that it can be told apart.
    char shown_nonce_[ui::kNonceSize] = {};
};

}  // namespace screens
