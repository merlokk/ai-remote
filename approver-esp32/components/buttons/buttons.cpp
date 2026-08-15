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
        state_[i].Reset(configs[i].active_low ? !level : level, now);
    }
    count_ = count;

    ESP_LOGI(TAG, "%u button(s) ready", static_cast<unsigned>(count_));
    return ESP_OK;
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
