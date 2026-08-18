#include "request_screen.h"

#include <cstdio>
#include <cstring>

#include "clock_screen.h"
#include "esp_log.h"

namespace screens {
namespace {

constexpr const char *TAG = "card";

// --- The palette ---------------------------------------------------------
// Black behind it, like everywhere else on this device (§10.8.1: an unlit pixel
// costs an AMOLED nothing). A card is up for at most a minute at a time so a
// lighter panel would not be a burn-in problem — it would be a *legibility*
// problem, because the whole point of the two plates is that they are the
// brightest things on the glass.
lv_color_t Ink() { return lv_color_make(236, 240, 238); }
lv_color_t Faint() { return lv_color_make(122, 130, 126); }
lv_color_t Rule() { return lv_color_make(52, 58, 55); }

// Amber for the header and the countdown: the one colour on this device that is
// neither the clock's green nor a verdict, so it cannot be mistaken for either.
lv_color_t Attention() { return lv_color_make(214, 158, 46); }

lv_color_t Allow() { return lv_color_make(0, 138, 74); }
lv_color_t Deny() { return lv_color_make(166, 46, 36); }

lv_obj_t *Bare(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h) {
    lv_obj_t *obj = lv_obj_create(parent);
    if (obj == nullptr) {
        return nullptr;
    }
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_outline_width(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(obj, 0, LV_PART_MAIN);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    return obj;
}

lv_obj_t *Text(lv_obj_t *parent, const lv_font_t *font, lv_color_t colour, int32_t x, int32_t y,
               const char *buffer) {
    lv_obj_t *label = lv_label_create(parent);
    if (label == nullptr) {
        return nullptr;
    }
    lv_obj_set_pos(label, x, y);
    lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, colour, LV_PART_MAIN);
    lv_label_set_text_static(label, buffer);
    return label;
}

// One of the two decision plates. **Built by the same function for both**, which
// is how §10.8.4's "the same size and weight" stays true after somebody edits
// one of them: there is only one of them to edit.
lv_obj_t *Plate(lv_obj_t *parent, int32_t x, lv_color_t colour, const char *key,
                const char *word) {
    lv_obj_t *plate = lv_obj_create(parent);
    if (plate == nullptr) {
        return nullptr;
    }
    const int32_t width = (480 - 2 * kCardPad - kCardPlateGap) / 2;
    lv_obj_set_pos(plate, x, kCardPlateTop);
    lv_obj_set_size(plate, width, kCardPlateHeight);
    lv_obj_set_style_bg_color(plate, colour, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(plate, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(plate, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(plate, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_all(plate, 0, LV_PART_MAIN);
    lv_obj_remove_flag(plate, LV_OBJ_FLAG_SCROLLABLE);
    // **Not clickable, deliberately** — see the header. The verdict is a physical
    // button, and a plate that could be tapped would put the reflex back.
    lv_obj_remove_flag(plate, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *name = lv_label_create(plate);
    lv_label_set_text_static(name, key);
    lv_obj_set_style_text_font(name, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(name, Ink(), LV_PART_MAIN);
    lv_obj_set_style_text_opa(name, LV_OPA_70, LV_PART_MAIN);
    lv_obj_align(name, LV_ALIGN_TOP_MID, 0, 10);

    lv_obj_t *verb = lv_label_create(plate);
    lv_label_set_text_static(verb, word);
    lv_obj_set_style_text_font(verb, &lv_font_montserrat_28, LV_PART_MAIN);
    lv_obj_set_style_text_color(verb, Ink(), LV_PART_MAIN);
    lv_obj_align(verb, LV_ALIGN_BOTTOM_MID, 0, -12);

    return plate;
}

const char *OutcomeWord(ui::Outcome outcome) {
    switch (outcome) {
        case ui::Outcome::kAllowed:
            return "ALLOWED";
        case ui::Outcome::kDenied:
            return "DENIED";
        case ui::Outcome::kTimedOut:
            return "TIMED OUT";
        case ui::Outcome::kNone:
            break;
    }
    return "";
}

lv_color_t OutcomeColour(ui::Outcome outcome) {
    switch (outcome) {
        case ui::Outcome::kAllowed:
            return lv_color_make(0, 186, 100);
        case ui::Outcome::kDenied:
            return lv_color_make(214, 66, 52);
        case ui::Outcome::kTimedOut:
            // Amber, not red: nothing was sent and nobody decided anything, which
            // is a different fact from a deny and must not be dressed as one
            // (§10.10 — and `request_card.h` has the same note).
            return lv_color_make(214, 158, 46);
        case ui::Outcome::kNone:
            break;
    }
    return lv_color_make(122, 130, 126);
}

}  // namespace

esp_err_t RequestScreen::Create(lv_obj_t *parent) {
    if (parent == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (root_ != nullptr) {
        return ESP_OK;
    }

    root_ = Bare(parent, 0, 0, 480, 480);
    if (root_ == nullptr) {
        ESP_LOGE(TAG, "out of LVGL memory building the card");
        return ESP_ERR_NO_MEM;
    }
    // Opaque black, because it has to *cover* the clock rather than share the
    // glass with it: a countdown over a drifting clock face is two things asking
    // to be read at once.
    lv_obj_set_style_bg_color(root_, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(root_, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);

    // **Clickable so that it swallows, not so that it answers.** §10.8.4: nothing
    // on this card is touchable, and the settings list of §10.8.5 is the first
    // screen with anything underneath worth hitting — an overlay that is not
    // clickable is one LVGL hit-tests straight through, so a finger aimed at a
    // card it could not answer would press a row on the screen behind it. There
    // is no handler on this object: a press lands here and stops.
    lv_obj_add_flag(root_, LV_OBJ_FLAG_CLICKABLE);

    card_ = Bare(root_, 0, 0, 480, 480);
    receipt_ = Bare(root_, 0, 0, 480, 480);
    if (card_ == nullptr || receipt_ == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    // --- the card ---------------------------------------------------------
    title_ = lv_label_create(card_);
    lv_label_set_text_static(title_, "PERMISSION REQUEST");
    lv_obj_set_style_text_font(title_, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(title_, Attention(), LV_PART_MAIN);
    lv_obj_set_pos(title_, kCardPad, kCardHeaderTop + 6);

    countdown_ = Text(card_, &lv_font_montserrat_28, Attention(), 0, kCardHeaderTop,
                      countdown_text_);
    waiting_ = Text(card_, &lv_font_montserrat_14, Faint(), 0, kCardHeaderTop + 6, waiting_text_);
    tool_ = Text(card_, &lv_font_montserrat_28, Ink(), kCardPad, kCardToolTop, tool_text_);
    cwd_ = Text(card_, &lv_font_montserrat_14, Faint(), kCardPad, kCardCwdTop, cwd_text_);
    if (countdown_ == nullptr || waiting_ == nullptr || tool_ == nullptr || cwd_ == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    // The path is the one field allowed to wrap rather than be refused: its tail
    // is a directory name, not an argument somebody is approving.
    lv_label_set_long_mode(cwd_, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(cwd_, 480 - 2 * kCardPad);

    rule_ = Bare(card_, kCardPad, kCardRuleTop, 480 - 2 * kCardPad, 2);
    if (rule_ == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    lv_obj_set_style_bg_color(rule_, Rule(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(rule_, LV_OPA_COVER, LV_PART_MAIN);

    // **The heaviest thing on the panel, and the only thing that scrolls**
    // (§10.8.4). Vertical only: a command that could be scrolled sideways is a
    // command with a hidden end.
    input_box_ = Bare(card_, kCardPad, kCardInputTop, 480 - 2 * kCardPad, kCardInputHeight);
    if (input_box_ == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    lv_obj_add_flag(input_box_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(input_box_, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(input_box_, LV_SCROLLBAR_MODE_AUTO);

    input_ = Text(input_box_, &lv_font_montserrat_28, Ink(), 0, 0, input_text_);
    if (input_ == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    lv_label_set_long_mode(input_, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(input_, 480 - 2 * kCardPad - 8);

    const int32_t plate_width = (480 - 2 * kCardPad - kCardPlateGap) / 2;
    allow_plate_ = Plate(card_, kCardPad, Allow(), "BOOT", "ALLOW");
    deny_plate_ = Plate(card_, kCardPad + plate_width + kCardPlateGap, Deny(), "PWR", "DENY");
    if (allow_plate_ == nullptr || deny_plate_ == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    // --- the receipt ------------------------------------------------------
    outcome_ = Text(receipt_, &lv_font_montserrat_28, Ink(), 0, 0, outcome_text_);
    receipt_tool_ = Text(receipt_, &lv_font_montserrat_28, Faint(), 0, 0, receipt_tool_text_);
    note_ = Text(receipt_, &lv_font_montserrat_14, Faint(), 0, 0, note_text_);
    if (outcome_ == nullptr || receipt_tool_ == nullptr || note_ == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    lv_obj_align(outcome_, LV_ALIGN_CENTER, 0, -40);
    lv_obj_align(receipt_tool_, LV_ALIGN_CENTER, 0, 4);
    lv_obj_align(note_, LV_ALIGN_CENTER, 0, 48);
    lv_obj_add_flag(receipt_, LV_OBJ_FLAG_HIDDEN);

    return ESP_OK;
}

void RequestScreen::Hide() {
    lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);
    shown_nonce_[0] = '\0';
    shown_seconds_ = 0xFFFFFFFFu;
    shown_waiting_ = 0xFF;
}

void RequestScreen::ShowCard(const ui::Request &request, uint32_t remaining_ms, uint8_t waiting) {
    const bool same = std::strncmp(shown_nonce_, request.nonce, sizeof(shown_nonce_)) == 0;
    if (!same) {
        std::snprintf(tool_text_, sizeof(tool_text_), "%s", request.tool_name);
        std::snprintf(cwd_text_, sizeof(cwd_text_), "%s", request.cwd);
        std::snprintf(input_text_, sizeof(input_text_), "%s", request.tool_input);
        lv_label_set_text_static(tool_, tool_text_);
        lv_label_set_text_static(cwd_, cwd_text_);
        lv_label_set_text_static(input_, input_text_);
        // A new command starts at its beginning. Inheriting the last card's scroll
        // position would show the middle of a command whose head nobody read.
        lv_obj_scroll_to_y(input_box_, 0, LV_ANIM_OFF);
        std::snprintf(shown_nonce_, sizeof(shown_nonce_), "%s", request.nonce);
    }

    // **Its own comparison, not the card's.** A request arriving behind this one
    // changes the queue and not the card, so this used to be inside the block
    // above and a queue that grew under a card was invisible.
    if (waiting != shown_waiting_) {
        shown_waiting_ = waiting;
        if (waiting > 0) {
            std::snprintf(waiting_text_, sizeof(waiting_text_), "+%u waiting",
                          static_cast<unsigned>(waiting));
        } else {
            waiting_text_[0] = '\0';
        }
        lv_label_set_text_static(waiting_, waiting_text_);
        lv_obj_align(waiting_, LV_ALIGN_TOP_RIGHT, -kCardPad, kCardHeaderTop + 34);
    }

    // Whole seconds, so the one field that changes on its own repaints once a
    // second rather than at the tick rate.
    const uint32_t seconds = (remaining_ms + 999) / 1000;
    if (seconds != shown_seconds_) {
        shown_seconds_ = seconds;
        std::snprintf(countdown_text_, sizeof(countdown_text_), "%u:%02u",
                      static_cast<unsigned>(seconds / 60), static_cast<unsigned>(seconds % 60));
        lv_label_set_text_static(countdown_, countdown_text_);
        lv_obj_align(countdown_, LV_ALIGN_TOP_RIGHT, -kCardPad, kCardHeaderTop);
    }

    lv_obj_remove_flag(card_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(receipt_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(root_, LV_OBJ_FLAG_HIDDEN);
    // Over whatever is behind it, every time: the clock is a sibling and LVGL
    // draws siblings in order (§10.8.1 — the card outranks everything).
    lv_obj_move_foreground(root_);
}

void RequestScreen::ShowReceipt(const ui::RequestCard &card, const char *note) {
    std::snprintf(outcome_text_, sizeof(outcome_text_), "%s", OutcomeWord(card.LastOutcome()));
    std::snprintf(receipt_tool_text_, sizeof(receipt_tool_text_), "%s", card.LastTool());

    // **The caller's note is about a decision, so a timeout does not get it.**
    // The first version printed it under every outcome and a card nobody touched
    // therefore read "decided, not sent" — which is the one thing on this screen
    // that must never be claimed (§10.10: never a silent allow, and never a silent
    // anything else either). Found by expiring a card on the board and looking at
    // the picture; the model had it right and the words did not.
    if (card.LastOutcome() == ui::Outcome::kTimedOut) {
        std::snprintf(note_text_, sizeof(note_text_), "nobody answered - nothing was sent");
    } else {
        std::snprintf(note_text_, sizeof(note_text_), "%s", note != nullptr ? note : "");
    }

    lv_label_set_text_static(outcome_, outcome_text_);
    lv_label_set_text_static(receipt_tool_, receipt_tool_text_);
    lv_label_set_text_static(note_, note_text_);
    lv_obj_set_style_text_color(outcome_, OutcomeColour(card.LastOutcome()), LV_PART_MAIN);
    lv_obj_align(outcome_, LV_ALIGN_CENTER, 0, -40);
    lv_obj_align(receipt_tool_, LV_ALIGN_CENTER, 0, 4);
    lv_obj_align(note_, LV_ALIGN_CENTER, 0, 48);

    lv_obj_add_flag(card_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(receipt_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(root_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(root_);
    shown_nonce_[0] = '\0';
    shown_waiting_ = 0xFF;
}

void RequestScreen::Apply(const ui::RequestCard &card, uint32_t now_ms, const char *note) {
    if (root_ == nullptr) {
        return;
    }

    const ui::CardState state = card.State();
    const bool state_changed = state != shown_;

    switch (state) {
        case ui::CardState::kIdle:
            if (state_changed) {
                Hide();
            }
            break;
        case ui::CardState::kCard: {
            const ui::Request *front = card.Front();
            if (front != nullptr) {
                ShowCard(*front, card.RemainingMs(now_ms), card.Waiting());
            }
            break;
        }
        case ui::CardState::kReceipt:
            if (state_changed) {
                ShowReceipt(card, note);
            }
            break;
    }

    shown_ = state;
}

}  // namespace screens
