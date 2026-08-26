// The peripheral and the task behind `led.h` (CLAUDE.md §10.17).
//
// Everything with a rule in it is in `led_frames.cpp`, which has no ESP-IDF and
// is run by §10.11's host tier. What is left here is a UART, a mutex, and a
// sleep that is as long as the animator says it may be.

#include "led.h"

#include <cstring>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

namespace led {
namespace {

constexpr const char *TAG = "led";

// 3,333,333 baud, six data bits: eight bit-times of 300 ns per character, which
// is two WS2812 bit cells (§10.17.1). The divisor off the 80 MHz APB clock is
// exactly 24, so there is no rounding error to accumulate over twelve bytes.
constexpr int kBaudRate = 3333333;

// **A receive buffer for a line that is only ever driven, and no transmit
// buffer at all.** Both halves are the driver's rules rather than a design:
//
//   * `uart_driver_install` refuses a receive buffer of zero and anything at or
//     below the 128-byte hardware FIFO — it answers `uart rx buffer length
//     error`, which is exactly how this was found, on the board, with the LED
//     staying dark and one line in the boot log. Nothing here ever reads, so
//     this is 256 bytes of RAM spent to satisfy a check;
//   * a transmit buffer of **zero** makes `uart_write_bytes` blocking, which is
//     what this wants. Twelve bytes at 3.33 Mbaud is 29 microseconds; a ring
//     buffer would add an interrupt and a copy to save a delay shorter than the
//     scheduler's tick.
//
// The same two numbers the house firmware of §10.14.4 uses, and for the same
// reasons — which is worth recording, because "why is the receive buffer bigger
// than the transmit one on a write-only line" is a question that looks like a
// mistake until it is answered.
constexpr int kRxBufferBytes = 256;
constexpr int kTxBufferBytes = 0;

// How long to wait for the twelve bytes to leave. At 3.33 Mbaud they take about
// 29 us; two ticks is four orders of magnitude of margin, and the only way to
// spend it is a driver that has stopped.
constexpr TickType_t kTxWaitTicks = pdMS_TO_TICKS(20);

struct State {
    bool ready = false;
    uart_port_t port = UART_NUM_1;
    gpio_num_t pin = GPIO_NUM_NC;

    Animator animator;
    uint8_t percent = 40;
    uint8_t idle_percent = 8;

    Rgb last_written{255, 255, 255};  // deliberately not a colour anybody sets
    bool ever_written = false;

    uint32_t writes = 0;
    uint32_t write_failures = 0;

    TaskHandle_t task = nullptr;
    SemaphoreHandle_t lock = nullptr;
    StaticSemaphore_t lock_storage{};
};

State state;

// **Static, because §10.14.1 allows no heap** — including the driver's. A
// `xSemaphoreCreateMutex` would allocate, and there is exactly one of these for
// the life of the device.
StackType_t task_stack[kTaskStackBytes / sizeof(StackType_t)];
StaticTask_t task_storage;

uint32_t NowMs() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }

bool Lock() {
    return state.lock != nullptr && xSemaphoreTake(state.lock, portMAX_DELAY) == pdTRUE;
}

void Unlock() {
    if (state.lock != nullptr) {
        xSemaphoreGive(state.lock);
    }
}

// Puts one pixel on the wire. Called only from the task, and only under no lock
// — the encoding needs nothing shared and the UART has its own.
bool Write(Rgb colour) {
    uint8_t frame[kBytesPerPixel];
    if (EncodePixel(colour, frame, sizeof(frame)) != kBytesPerPixel) {
        return false;
    }
    const int written = uart_write_bytes(state.port, frame, sizeof(frame));
    if (written != static_cast<int>(sizeof(frame))) {
        return false;
    }
    // **Waited on rather than fired and forgotten.** The next write must not
    // start inside the reset gap the part uses to latch — WS2812 latches on 50 us
    // of idle line, and a second frame that arrives before that is a second pixel
    // rather than a new colour for the first.
    return uart_wait_tx_done(state.port, kTxWaitTicks) == ESP_OK;
}

void Task(void *) {
    for (;;) {
        uint32_t next_ms = kRefreshMs;
        Rgb frame{};
        bool force = false;

        if (Lock()) {
            frame = state.animator.FrameAt(NowMs(), &next_ms);
            force = !state.ever_written;
            Unlock();
        }

        // Refresh even when nothing changed, but only at the ceiling — that is
        // what `kRefreshMs` buys, and it is why `next_ms` is clamped rather than
        // obeyed when it is long.
        if (next_ms > kRefreshMs) {
            next_ms = kRefreshMs;
        }

        if (force || frame != state.last_written) {
            if (Write(frame)) {
                state.last_written = frame;
                state.ever_written = true;
                state.writes++;
            } else {
                state.write_failures++;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(next_ms));
    }
}

}  // namespace

esp_err_t Init(uart_port_t port, gpio_num_t pin) {
    if (state.ready) {
        return ESP_OK;
    }
    if (pin == GPIO_NUM_NC) {
        return ESP_ERR_INVALID_ARG;
    }

    state.port = port;
    state.pin = pin;

    const uart_config_t cfg = {
        .baud_rate = kBaudRate,
        .data_bits = UART_DATA_6_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .rx_flow_ctrl_thresh = 0,
        .source_clk = UART_SCLK_APB,
        .flags = {},
    };

    esp_err_t err = uart_driver_install(port, kRxBufferBytes, kTxBufferBytes, 0, nullptr, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_driver_install: %s", esp_err_to_name(err));
        return err;
    }
    err = uart_param_config(port, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_param_config: %s", esp_err_to_name(err));
        return err;
    }
    err = uart_set_pin(port, pin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_set_pin: %s", esp_err_to_name(err));
        return err;
    }
    // **The inversion is the trick, not a polarity preference** (§10.17.1). A
    // UART idles its TX line high; WS2812 wants it low between frames, and the
    // four encoding characters are chosen for the inverted line. Without this
    // line the part sees a permanent reset condition and nothing else.
    err = uart_set_line_inverse(port, UART_SIGNAL_TXD_INV);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_set_line_inverse: %s", esp_err_to_name(err));
        return err;
    }

    state.lock = xSemaphoreCreateMutexStatic(&state.lock_storage);
    if (state.lock == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    // Dark until somebody asks for something. A device whose first act is to
    // flash a colour it does not mean is a device whose LED cannot be trusted as
    // a readout, which on this board is the whole readout.
    state.animator.Set(colour::kOff, Effect::kSolid, 0, NowMs());

    state.task = xTaskCreateStatic(Task, "led", sizeof(task_stack) / sizeof(StackType_t), nullptr,
                                   kTaskPriority, task_stack, &task_storage);
    if (state.task == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    state.ready = true;
    ESP_LOGI(TAG, "WS2812 on GPIO%d via UART%d, %d baud", static_cast<int>(pin),
             static_cast<int>(port), kBaudRate);
    return ESP_OK;
}

bool Ready() { return state.ready; }

void SetBrightness(uint8_t percent, uint8_t idle_percent) {
    if (percent > 100) {
        percent = 100;
    }
    if (idle_percent > 100) {
        idle_percent = 100;
    }
    if (!Lock()) {
        return;
    }
    const bool changed = percent != state.percent || idle_percent != state.idle_percent;
    state.percent = percent;
    state.idle_percent = idle_percent;
    if (changed) {
        // Re-assert at the new ceiling. Without this a brightness change would
        // wait for the next state change to be visible, and on an idle device
        // that could be a long time — which reads as a setting that did nothing.
        const Rgb colour = state.animator.Colour();
        const Effect effect = state.animator.CurrentEffect();
        const uint8_t was = state.animator.Percent();
        const uint8_t now = (was == state.idle_percent || was == 0) ? idle_percent : percent;
        state.animator.Set(colour, effect, now, NowMs());
    }
    Unlock();
    if (state.task != nullptr) {
        xTaskAbortDelay(state.task);
    }
}

void Set(Rgb colour, Effect effect) {
    if (!Lock()) {
        return;
    }
    state.animator.Set(colour, effect, state.percent, NowMs());
    Unlock();
    if (state.task != nullptr) {
        xTaskAbortDelay(state.task);
    }
}

void SetIdle(Rgb colour, Effect effect) {
    if (!Lock()) {
        return;
    }
    state.animator.Set(colour, effect, state.idle_percent, NowMs());
    Unlock();
    if (state.task != nullptr) {
        xTaskAbortDelay(state.task);
    }
}

void SetFor(Rgb colour, Effect effect, uint32_t duration_ms) {
    if (!Lock()) {
        return;
    }
    state.animator.SetFor(colour, effect, state.percent, duration_ms, NowMs());
    Unlock();
    if (state.task != nullptr) {
        xTaskAbortDelay(state.task);
    }
}

void EndFor() {
    if (!Lock()) {
        return;
    }
    state.animator.EndFor(NowMs());
    Unlock();
    if (state.task != nullptr) {
        xTaskAbortDelay(state.task);
    }
}

void Off() { Set(colour::kOff, Effect::kSolid); }

Status Get() {
    Status out;
    out.ready = state.ready;
    out.percent = state.percent;
    out.idle_percent = state.idle_percent;
    out.writes = state.writes;
    out.write_failures = state.write_failures;
    if (Lock()) {
        out.colour = state.animator.Colour();
        out.effect = state.animator.CurrentEffect();
        out.overriding = state.animator.Overriding();
        Unlock();
    }
    if (state.task != nullptr) {
        out.stack_low_water = uxTaskGetStackHighWaterMark(state.task);
    }
    return out;
}

}  // namespace led
