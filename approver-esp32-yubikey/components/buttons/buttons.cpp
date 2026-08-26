#include "buttons.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace buttons {

namespace {

constexpr const char *TAG = "buttons";

uint32_t NowMs() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }

}  // namespace

void Debounce::Reset(bool pressed, uint32_t now_ms) {
    pressed_ = pressed;
    candidate_ = pressed;
    candidate_since_ms_ = now_ms;
    changed_at_ms_ = now_ms;
}

Event Debounce::Update(bool raw_pressed, uint32_t now_ms) {
    // Any change in the raw level restarts the window. A bouncing contact never
    // holds one level long enough to get past it, which is the whole trick.
    if (raw_pressed != candidate_) {
        candidate_ = raw_pressed;
        candidate_since_ms_ = now_ms;
    }

    if (raw_pressed == pressed_) {
        return Event::kNone;
    }
    if (now_ms - candidate_since_ms_ < kDebounceMs) {
        return Event::kNone;
    }

    pressed_ = raw_pressed;
    changed_at_ms_ = now_ms;
    if (pressed_) {
        // Remembered as well as reported. See `TakePress`: the reader is not the
        // poller on this board.
        press_latched_ = true;
    }
    return pressed_ ? Event::kPressed : Event::kReleased;
}

esp_err_t Buttons::Init(const Config *configs, size_t count) {
    if (configs == nullptr || count == 0 || count > kMaxButtons) {
        return ESP_ERR_INVALID_ARG;
    }

    gpio_config_t io = {};
    io.mode = GPIO_MODE_INPUT;
    io.intr_type = GPIO_INTR_DISABLE;

    // One gpio_config per button rather than one mask for all of them: the
    // pull-up is per button in `Config`, and a shared mask would quietly apply
    // the first one's choice to the rest.
    for (size_t i = 0; i < count; ++i) {
        if (configs[i].pin == GPIO_NUM_NC) {
            return ESP_ERR_INVALID_ARG;
        }
        io.pin_bit_mask = 1ULL << static_cast<uint32_t>(configs[i].pin);
        io.pull_up_en = configs[i].pull_up ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE;
        io.pull_down_en = GPIO_PULLDOWN_DISABLE;
        const esp_err_t err = gpio_config(&io);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "%s (GPIO%d) not configured: %s", configs[i].name,
                     static_cast<int>(configs[i].pin), esp_err_to_name(err));
            return err;
        }
    }

    const uint32_t now = NowMs();
    for (size_t i = 0; i < count; ++i) {
        configs_[i] = configs[i];
        // Adopt whatever the pin says right now instead of assuming released:
        // §10.15's restore is a button that is *already* held when this runs.
        const bool level = gpio_get_level(configs[i].pin) != 0;
        const bool pressed = configs[i].active_low ? !level : level;
        state_[i].Reset(pressed, now);
        // The latch adopts it too, so a finger already down at boot is not
        // collected later as a press somebody made.
        latch_[i].Reset(pressed, now);
    }
    count_ = count;

    // **The poller** (see the header). Static storage and a task that outlives
    // everything, like the rest of this firmware (§10.14.1). It reads GPIOs and
    // nothing else, so the stack is the FreeRTOS minimum plus room for the log
    // line that never happens.
    static StackType_t poller_stack[1536 / sizeof(StackType_t)];
    static StaticTask_t poller_tcb;
    if (xTaskCreateStatic(&PollerTask, "buttons", sizeof poller_stack / sizeof *poller_stack, this,
                          1, poller_stack, &poller_tcb) == nullptr) {
        // Not fatal, and deliberately so: without it the console still reads the
        // pins and everything except the deny button works (§10.10 — the safe
        // direction is a device that cannot deny, not one that cannot refuse).
        ESP_LOGE(TAG, "the button poller did not start - BOOT will not deny");
    }

    ESP_LOGI(TAG, "%u button(s) ready", static_cast<unsigned>(count_));
    return ESP_OK;
}

void Buttons::PollerTask(void *arg) {
    Buttons *self = static_cast<Buttons *>(arg);
    for (;;) {
        const uint32_t now = NowMs();
        for (size_t i = 0; i < self->count_; ++i) {
            self->latch_[i].Update(self->RawPressed(i), now);
        }
        vTaskDelay(pdMS_TO_TICKS(kPollIntervalMs));
    }
}

bool Buttons::TakePress(size_t index) {
    return Valid(index) && latch_[index].TakePress();
}

const char *Buttons::Name(size_t index) const {
    return Valid(index) ? configs_[index].name : "";
}

gpio_num_t Buttons::Gpio(size_t index) const {
    return Valid(index) ? configs_[index].pin : GPIO_NUM_NC;
}

bool Buttons::RawPressed(size_t index) const {
    if (!Valid(index)) {
        return false;
    }
    const bool level = gpio_get_level(configs_[index].pin) != 0;
    return configs_[index].active_low ? !level : level;
}

Event Buttons::Poll(size_t index) {
    if (!Valid(index)) {
        return Event::kNone;
    }
    return state_[index].Update(RawPressed(index), NowMs());
}

void Buttons::PollAll() {
    const uint32_t now = NowMs();
    for (size_t i = 0; i < count_; ++i) {
        state_[i].Update(RawPressed(i), now);
    }
}

bool Buttons::Pressed(size_t index) const {
    return Valid(index) && state_[index].Pressed();
}

uint32_t Buttons::StableMs(size_t index) const {
    return Valid(index) ? state_[index].StableMs(NowMs()) : 0;
}

Press PressLength::Update(bool pressed, uint32_t now_ms) {
    if (pressed) {
        if (!down_) {
            down_ = true;
            fired_ = false;
            down_at_ms_ = now_ms;
        }
        if (!fired_ && (now_ms - down_at_ms_) >= long_ms_) {
            fired_ = true;
            return Press::kLong;
        }
        return Press::kNone;
    }

    // Released. A press already reported as long says nothing on the way out —
    // see the header: one press, one action.
    const bool was_short = down_ && !fired_;
    down_ = false;
    fired_ = false;
    return was_short ? Press::kShort : Press::kNone;
}

uint32_t PressLength::HeldMs(uint32_t now_ms) const {
    return down_ ? now_ms - down_at_ms_ : 0;
}

bool Buttons::HeldFor(size_t index, uint32_t hold_ms) {
    if (!Valid(index)) {
        return false;
    }

    const uint32_t started = NowMs();
    while (true) {
        Poll(index);
        if (!state_[index].Pressed()) {
            return false;
        }
        if (NowMs() - started >= hold_ms) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(kPollIntervalMs));
    }
}

}  // namespace buttons
