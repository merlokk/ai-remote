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

    // **What the session is doing** (§9.10), at the size the numbers are and
    // truncated rather than wrapped: a second line here would push the session off
    // the panel, and the whole summary is a `limits` away on the console.
    activity_ = Text(root_, &lv_font_montserrat_28, Bright(), kLimitsPad, kLimitsActivityTop,
                     activity_text_);
    if (activity_ != nullptr) {
        lv_obj_set_width(activity_, kBarWidth);
        // **SCROLL, and the two modes it was chosen over are both wrong for a
        // reason worth writing down.**
        //
        // `LV_LABEL_LONG_MODE_DOTS` writes its ellipsis *into the text buffer*, so
        // `lv_label_set_dots` returns immediately for a static string
        // (`lv_label.c`: `if(label->static_txt != 0) return;`) — the mode would be
        // set, do nothing, and leave a line drawing past its own width. Every
        // string on this screen is static, for the reason §10.14.1 gives.
        //
        // `CLIP` was what this had first, and it ends a cut line at the panel edge
        // with nothing to say it was cut: `PowerShell - cd E:\projects\ai-` reads
        // as a command that ended there. A readout that cannot be told from a
        // shorter true one is the one thing this screen must not be (§10.8.3).
        //
        // So it scrolls, back and forth, and only when it does not fit — a line
        // that fits is drawn still. On an AMOLED that is the *cheap* direction to
        // be wrong in: §10.8.2 moves the clock's digits around the panel on
        // purpose, and this row moving for a few seconds after each tool call is
        // the same medicine.
        lv_label_set_long_mode(activity_, LV_LABEL_LONG_MODE_SCROLL);
    }

    cwd_ = Text(root_, &lv_font_montserrat_14, Faint(), kLimitsPad, kLimitsCwdTop, cwd_text_);

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

void LimitsScreen::ApplyActivity(const ui::ActivityView &activity, uint32_t now_ms) {
    if (activity_ == nullptr) {
        return;
    }
    if (!activity.HasDocument()) {
        // Nothing has arrived on §9.10's subject since boot: an empty row rather
        // than a placeholder, because the screen above it is already saying the
        // session is there and a line reading "nothing" would argue with it.
        if (shown_activity_ != 0xFFFFFFFFu) {
            shown_activity_ = 0xFFFFFFFFu;
            activity_text_[0] = '\0';
            lv_label_set_text_static(activity_, activity_text_);
        }
        return;
    }

    // **Faint is the state**, and it is the whole of what this line says beyond
    // its text: bright while there is work, faint for a turn that has ended or a
    // document too old to believe (`ui/activity_view.h` argues both). No red — a
    // busy session is not a problem, and red on this screen belongs to a gauge.
    const ui::Activity &document = activity.Document();
    const bool faint =
        document.state == ui::ActivityState::kIdle || activity.Stale(now_ms);

    if (activity.Received() != shown_activity_) {
        shown_activity_ = activity.Received();
        activity.Headline(activity_text_, sizeof activity_text_);
        lv_label_set_text_static(activity_, activity_text_);
    }
    if (faint != shown_activity_faint_) {
        shown_activity_faint_ = faint;
        lv_obj_set_style_text_color(activity_, faint ? Faint() : Bright(), LV_PART_MAIN);
    }
}

void LimitsScreen::Apply(const ui::LimitsView &view, const ui::ActivityView &activity,
                         int64_t epoch_now, uint32_t now_ms) {
    if (root_ == nullptr || !visible_) {
        return;
    }
    // The line is applied first and unconditionally: §9.10's documents and §9.7's
    // come from different publishers, and a session whose activity has arrived
    // while its numbers have not is an ordinary state rather than a reason to draw
    // neither.
    ApplyActivity(activity, now_ms);

    if (!view.HasDocument()) {
        return;
    }

    const ui::Limits &limits = view.Document();
    const uint32_t seconds = view.AgeMs(now_ms) / 1000;

    // Read once, because two blocks below ask it and the second would always
    // answer "no" if the first had already stamped the counter.
    const bool fresh = view.Received() != shown_received_;
    shown_received_ = view.Received();

    // The header only changes when a new document lands, which on a working
    // session is every few seconds and never per frame.
    if (fresh) {
        std::snprintf(model_text_, sizeof model_text_, "%s",
                      limits.model[0] != '\0' ? limits.model : "unknown model");
        lv_label_set_text_static(model_, model_text_);

        if (limits.effort[0] != '\0') {
            std::snprintf(effort_text_, sizeof effort_text_, "effort %s", limits.effort);
        } else {
            effort_text_[0] = '\0';
        }
        lv_label_set_text_static(effort_, effort_text_);
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

    // **The session and the age, on one line** — the directory because without it
    // the screen reads as belonging to whatever request is on the card and every
    // session on the machine publishes here (§10.8.3), and the age because these
    // numbers are a current value with no stream behind it (§9.7): they are as true
    // as they are recent, and nothing else here would say so.
    //
    // One buffer, so either half changing rewrites it — which for the age is once a
    // second and for the directory is once a document.
    if (seconds != shown_seconds_ || fresh) {
        shown_seconds_ = seconds;
        std::snprintf(cwd_text_, sizeof cwd_text_, "%s  -  %u s ago",
                      limits.cwd[0] != '\0' ? limits.cwd : "session directory unknown",
                      static_cast<unsigned>(seconds));
        lv_label_set_text_static(cwd_, cwd_text_);
    }
}

}  // namespace screens
