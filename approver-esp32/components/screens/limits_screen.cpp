#include "limits_screen.h"

#include <cstdio>
#include <cstring>

namespace screens {
namespace {

// The same palette the other two screens use, plus the traffic light of §9.2 —
// which is three colours this device did not have, and they are deliberately the
// *same* three the terminal line uses so that a glance at the desk and a glance at
// the status bar mean the same thing.
lv_color_t Bright() { return lv_color_make(214, 222, 216); }
lv_color_t Faint() { return lv_color_make(122, 130, 126); }
lv_color_t Rule() { return lv_color_make(52, 58, 55); }

lv_color_t Green() { return lv_color_make(0, 158, 84); }
lv_color_t Yellow() { return lv_color_make(214, 158, 46); }
lv_color_t Red() { return lv_color_make(190, 56, 44); }

lv_color_t LevelColour(ui::Level level) {
    switch (level) {
        case ui::Level::kGreen:
            return Green();
        case ui::Level::kYellow:
            return Yellow();
        case ui::Level::kRed:
            return Red();
    }
    return Faint();
}

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

// A filled rectangle. The bar is two of them — a dark track and a coloured fill
// whose *width* is the number — rather than an LVGL bar widget, for the reason
// §10.8.2 gives about the clock's digits: a widget brings a style tree and an
// animation engine to draw something that is a rectangle.
lv_obj_t *Block(lv_obj_t *parent, lv_color_t colour, int32_t x, int32_t y, int32_t w, int32_t h) {
    lv_obj_t *obj = Bare(parent, x, y, w, h);
    if (obj == nullptr) {
        return nullptr;
    }
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(obj, colour, LV_PART_MAIN);
    lv_obj_set_style_radius(obj, 3, LV_PART_MAIN);
    return obj;
}

constexpr int32_t kWidth = 480;
constexpr int32_t kBarWidth = kWidth - 2 * kLimitsPad;

}  // namespace

esp_err_t LimitsScreen::Create(lv_obj_t *parent) {
    if (parent == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    root_ = Bare(parent, 0, 0, kWidth, kWidth);
    if (root_ == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);

    std::snprintf(model_text_, sizeof model_text_, "%s", "no session");
    model_ = Text(root_, &lv_font_montserrat_28, Bright(), kLimitsPad, kLimitsModelTop, model_text_);
    effort_ =
        Text(root_, &lv_font_montserrat_14, Faint(), kLimitsPad, kLimitsEffortTop, effort_text_);

    struct Row {
        GaugeRow *row;
        const char *label;
        int32_t top;
    };
    const Row rows[] = {
        {&five_hour_, "5h", kLimitsFirstGaugeTop},
        {&seven_day_, "7d", kLimitsFirstGaugeTop + kLimitsGaugeStride},
        {&context_, "ctx", kLimitsFirstGaugeTop + 2 * kLimitsGaugeStride},
    };

    for (const Row &row : rows) {
        // The label is static text and never changes, so it is the one string here
        // that LVGL may keep its own copy of.
        lv_obj_t *label = lv_label_create(root_);
        if (label != nullptr) {
            lv_obj_set_pos(label, kLimitsPad, row.top + kLimitsTextTop);
            lv_obj_set_style_text_font(label, &lv_font_montserrat_28, LV_PART_MAIN);
            lv_obj_set_style_text_color(label, Faint(), LV_PART_MAIN);
            lv_label_set_text(label, row.label);
        }
        row.row->label = label;

        std::snprintf(row.row->percent_text, sizeof row.row->percent_text, "%s", "--");
        row.row->percent = Text(root_, &lv_font_montserrat_28, Bright(), kLimitsPercentLeft,
                                row.top + kLimitsTextTop, row.row->percent_text);

        // Right-aligned against the same margin the bar ends at, so a countdown
        // that grows a character grows leftwards instead of walking off the panel.
        row.row->countdown =
            Text(root_, &lv_font_montserrat_28, Faint(), kWidth - kLimitsPad - kLimitsCountdownWidth,
                 row.top + kLimitsTextTop, row.row->countdown_text);
        if (row.row->countdown != nullptr) {
            lv_obj_set_width(row.row->countdown, kLimitsCountdownWidth);
            lv_obj_set_style_text_align(row.row->countdown, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
        }

        row.row->track = Block(root_, Rule(), kLimitsPad, row.top + kLimitsBarTop, kBarWidth,
                               kLimitsBarHeight);
        row.row->fill =
            Block(root_, Green(), kLimitsPad, row.top + kLimitsBarTop, 1, kLimitsBarHeight);
    }

    cwd_ = Text(root_, &lv_font_montserrat_14, Faint(), kLimitsPad, kLimitsCwdTop, cwd_text_);
    age_ = Text(root_, &lv_font_montserrat_14, Rule(), kLimitsPad, kLimitsAgeTop, age_text_);

    return ESP_OK;
}

void LimitsScreen::SetVisible(bool visible) {
    if (root_ == nullptr || visible == visible_) {
        return;
    }
    visible_ = visible;
    if (visible) {
        lv_obj_remove_flag(root_, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);
    }
}

void LimitsScreen::ApplyGauge(GaugeRow *row, const ui::Gauge &gauge, ui::Level level,
                              const char *countdown) {
    if (row->fill == nullptr) {
        return;
    }

    if (!gauge.present) {
        // **Absent is not zero** (§9.7): an API key rather than a subscription has
        // no rate limits at all, and an empty bar would read as a fresh window.
        if (row->shown_present || row->shown_percent != 0xFF) {
            std::snprintf(row->percent_text, sizeof row->percent_text, "%s", "--");
            lv_obj_add_flag(row->fill, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text_static(row->percent, row->percent_text);
            row->shown_present = false;
            row->shown_percent = 0xFF;
        }
        if (row->countdown_text[0] != '\0') {
            row->countdown_text[0] = '\0';
            lv_label_set_text_static(row->countdown, row->countdown_text);
        }
        return;
    }

    if (!row->shown_present || row->shown_percent != gauge.used_percent) {
        row->shown_present = true;
        row->shown_percent = gauge.used_percent;

        std::snprintf(row->percent_text, sizeof row->percent_text, "%u%%",
                      static_cast<unsigned>(gauge.used_percent));
        lv_label_set_text_static(row->percent, row->percent_text);

        // At 0 % the fill is one pixel rather than nothing: a bar that vanishes
        // reads as a missing gauge, and this screen already has a way of saying
        // that.
        const int32_t width = 1 + (kBarWidth - 1) * gauge.used_percent / 100;
        lv_obj_set_width(row->fill, width);
        lv_obj_set_style_bg_color(row->fill, LevelColour(level), LV_PART_MAIN);
        lv_obj_remove_flag(row->fill, LV_OBJ_FLAG_HIDDEN);
    }

    if (std::strcmp(row->countdown_text, countdown) != 0) {
        std::snprintf(row->countdown_text, sizeof row->countdown_text, "%s", countdown);
        lv_label_set_text_static(row->countdown, row->countdown_text);
    }
}

void LimitsScreen::Apply(const ui::LimitsView &view, int64_t epoch_now, uint32_t now_ms) {
    if (root_ == nullptr || !visible_ || !view.HasDocument()) {
        return;
    }

    const ui::Limits &limits = view.Document();
    const uint32_t seconds = view.AgeMs(now_ms) / 1000;

    // The header only changes when a new document lands, which on a working
    // session is every few seconds and never per frame.
    if (view.Received() != shown_received_) {
        shown_received_ = view.Received();

        std::snprintf(model_text_, sizeof model_text_, "%s",
                      limits.model[0] != '\0' ? limits.model : "unknown model");
        lv_label_set_text_static(model_, model_text_);

        if (limits.effort[0] != '\0') {
            std::snprintf(effort_text_, sizeof effort_text_, "effort %s", limits.effort);
        } else {
            effort_text_[0] = '\0';
        }
        lv_label_set_text_static(effort_, effort_text_);

        // §10.8.3: without this line the screen reads as belonging to whatever
        // request is on the card, and every session on the machine publishes here.
        std::snprintf(cwd_text_, sizeof cwd_text_, "%s",
                      limits.cwd[0] != '\0' ? limits.cwd : "session directory unknown");
        lv_label_set_text_static(cwd_, cwd_text_);
    }

    char countdown[ui::kResetsTextSize];

    view.CountdownFor(limits.five_hour, epoch_now, now_ms, countdown, sizeof countdown);
    ApplyGauge(&five_hour_, limits.five_hour, ui::WindowLevel(limits.five_hour.used_percent),
               countdown);

    view.CountdownFor(limits.seven_day, epoch_now, now_ms, countdown, sizeof countdown);
    ApplyGauge(&seven_day_, limits.seven_day, ui::WindowLevel(limits.seven_day.used_percent),
               countdown);

    // The context window has no reset on a clock — it resets when the session
    // does — so it gets no countdown, and `CountdownFor` answers empty for it.
    view.CountdownFor(limits.context, epoch_now, now_ms, countdown, sizeof countdown);
    ApplyGauge(&context_, limits.context, ui::ContextLevel(limits.context.used_percent), countdown);

    // **The age, and it is the honest half of this screen.** These numbers are a
    // current value with no stream behind it (§9.7): they are as true as they are
    // recent, and nothing else here would say so.
    if (seconds != shown_seconds_) {
        shown_seconds_ = seconds;
        std::snprintf(age_text_, sizeof age_text_, "%u s ago", static_cast<unsigned>(seconds));
        lv_label_set_text_static(age_, age_text_);
    }
}

}  // namespace screens
