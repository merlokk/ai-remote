#include "fake_platform.h"

#include <cstdarg>
#include <cstdio>

#include "freertos/semphr.h"
#include "freertos/task.h"
#include "rom/ets_sys.h"

namespace fake {

namespace {

Platform platform;

}  // namespace

Platform &P() { return platform; }

// Everything in `Platform` is a plain value, so zeroing it is the whole reset —
// which is what the I²C fake's handle tables used to make untrue.
void Reset() { platform = {}; }

void AdvanceMs(uint32_t ms) { platform.clock_us += static_cast<uint64_t>(ms) * 1000; }

void SetPinLevel(gpio_num_t pin, int level) {
    const size_t index = static_cast<size_t>(pin);
    if (pin < 0 || index >= Platform::kMaxPins) {
        return;
    }
    platform.level[index] = level != 0 ? 1 : 0;
    platform.level_forced[index] = true;
}

size_t RisingEdges(gpio_num_t pin) {
    const size_t index = static_cast<size_t>(pin);
    if (pin < 0 || index >= Platform::kMaxPins) {
        return 0;
    }
    return platform.rising_edges[index];
}

void TakeMutexFromAnotherTask() { platform.mutex_taken = true; }

void GiveMutexFromAnotherTask() { platform.mutex_taken = false; }

void Log(const char *level, const char *tag, const char *format, ...) {
    if (!platform.log_enabled) {
        return;
    }
    std::printf("%s (%s) ", level, tag);
    va_list args;
    va_start(args, format);
    std::vprintf(format, args);
    va_end(args);
    std::printf("\n");
}

}  // namespace fake

// --- esp_err ---------------------------------------------------------------

const char *esp_err_to_name(esp_err_t code) {
    switch (code) {
        case ESP_OK: return "ESP_OK";
        case ESP_FAIL: return "ESP_FAIL";
        case ESP_ERR_NO_MEM: return "ESP_ERR_NO_MEM";
        case ESP_ERR_INVALID_ARG: return "ESP_ERR_INVALID_ARG";
        case ESP_ERR_INVALID_STATE: return "ESP_ERR_INVALID_STATE";
        case ESP_ERR_INVALID_SIZE: return "ESP_ERR_INVALID_SIZE";
        case ESP_ERR_NOT_FOUND: return "ESP_ERR_NOT_FOUND";
        case ESP_ERR_NOT_SUPPORTED: return "ESP_ERR_NOT_SUPPORTED";
        case ESP_ERR_TIMEOUT: return "ESP_ERR_TIMEOUT";
        default: return "ESP_ERR_?";
    }
}

// --- FreeRTOS --------------------------------------------------------------

SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *storage) {
    storage->created = true;
    storage->taken = false;
    return storage;
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t handle, TickType_t ticks) {
    fake::Platform &p = fake::P();
    p.last_take_ticks = ticks;
    ++p.take_calls;
    if (handle == nullptr || p.mutex_taken) {
        return pdFALSE;
    }
    p.mutex_taken = true;
    return pdTRUE;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t handle) {
    fake::Platform &p = fake::P();
    ++p.give_calls;
    if (handle == nullptr) {
        return pdFALSE;
    }
    p.mutex_taken = false;
    return pdTRUE;
}

void vTaskDelay(TickType_t ticks) {
    fake::Platform &p = fake::P();
    p.delay_ms_total += ticks;
    // A delay moves the clock. Without this, anything that polls in a loop
    // waiting for time to pass never finishes.
    p.clock_us += static_cast<uint64_t>(ticks) * 1000;
}

void esp_rom_delay_us(uint32_t us) { fake::P().clock_us += us; }

int64_t esp_timer_get_time(void) { return static_cast<int64_t>(fake::P().clock_us); }

// --- GPIO ------------------------------------------------------------------

esp_err_t gpio_config(const gpio_config_t *config) {
    fake::Platform &p = fake::P();
    for (size_t pin = 0; pin < fake::Platform::kMaxPins; ++pin) {
        if (((config->pin_bit_mask >> pin) & 1ULL) == 0) {
            continue;
        }
        p.last_mode[pin] = config->mode;
        // **The pull decides the idle level** — but only when nothing else
        // is driving the pin. A button held down shorts to ground and wins
        // over a pull-up, which is §10.15's scenario and would be silently
        // undone if configuring the pin reset it.
        if (p.level_forced[pin]) {
            continue;
        }
        if (config->pull_up_en == GPIO_PULLUP_ENABLE) {
            p.level[pin] = 1;
        } else if (config->pull_down_en == GPIO_PULLDOWN_ENABLE) {
            p.level[pin] = 0;
        }
    }
    return ESP_OK;
}

esp_err_t gpio_set_level(gpio_num_t pin, uint32_t level) {
    fake::Platform &p = fake::P();
    const size_t index = static_cast<size_t>(pin);
    if (pin < 0 || index >= fake::Platform::kMaxPins) {
        return ESP_ERR_INVALID_ARG;
    }
    // Rising edges, not calls: what a caller owes a line is *edges*, and
    // counting writes would let a driver that never went low pass.
    if (p.level[index] == 0 && level != 0) {
        ++p.rising_edges[index];
    }
    p.level[index] = level != 0 ? 1 : 0;
    return ESP_OK;
}

int gpio_get_level(gpio_num_t pin) {
    const size_t index = static_cast<size_t>(pin);
    if (pin < 0 || index >= fake::Platform::kMaxPins) {
        return 0;
    }
    return fake::P().level[index];
}
