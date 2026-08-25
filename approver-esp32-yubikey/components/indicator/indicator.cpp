// The task behind `indicator.h` (CLAUDE.md §10.17). Twenty lines of loop; the
// decisions are all in `indicator_policy.cpp`, which has no ESP-IDF in it.

#include "indicator.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led.h"

namespace indicator {
namespace {

constexpr const char *TAG = "ind";

struct Runtime {
    bool ready = false;
    Gatherer gatherer = nullptr;
    State current = State::kBooting;
    bool ever_applied = false;
    uint32_t transitions = 0;
    TaskHandle_t task = nullptr;
};

Runtime runtime;

// §10.14.1: no heap, including FreeRTOS's.
StackType_t task_stack[kTaskStackBytes / sizeof(StackType_t)];
StaticTask_t task_storage;

void Apply(State state) {
    const Look look = LookOf(state);
    if (look.idle) {
        led::SetIdle(look.colour, look.effect);
    } else {
        led::Set(look.colour, look.effect);
    }
}

void Task(void *) {
    for (;;) {
        Inputs inputs;
        if (runtime.gatherer != nullptr) {
            runtime.gatherer(&inputs);
        } else {
            // No gatherer yet is not "everything is broken" — it is `main` still
            // composing. Saying `kBooting` is the honest answer and it is what
            // the default-constructed struct already means.
            inputs.booting = true;
        }

        const State decided = Decide(inputs);
        if (decided != runtime.current || !runtime.ever_applied) {
            if (runtime.ever_applied) {
                ESP_LOGI(TAG, "%s -> %s (%s)", StateName(runtime.current), StateName(decided),
                         StateText(decided));
                runtime.transitions++;
            } else {
                ESP_LOGI(TAG, "%s (%s)", StateName(decided), StateText(decided));
            }
            runtime.current = decided;
            runtime.ever_applied = true;
        }

        // **Re-applied every tick, not only on a change.** `led::Set` is a no-op
        // when nothing moved (that check is in `Animator::Set`, and it is why
        // that call takes a clock), so this costs a comparison — and it buys the
        // case where a `SetFor` verdict flash has just expired and the state
        // underneath has to be put back without anything having changed.
        Apply(runtime.current);

        vTaskDelay(pdMS_TO_TICKS(kTickMs));
    }
}

}  // namespace

esp_err_t Init() {
    if (runtime.ready) {
        return ESP_OK;
    }
    if (!led::Ready()) {
        // Not fatal, and deliberately so (§10.10): a device whose LED did not
        // come up should still approve things. It just cannot say so.
        ESP_LOGW(TAG, "the LED is not up; the indicator will run and be invisible");
    }
    runtime.task = xTaskCreateStatic(Task, "indicator", sizeof(task_stack) / sizeof(StackType_t),
                                     nullptr, kTaskPriority, task_stack, &task_storage);
    if (runtime.task == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    runtime.ready = true;
    return ESP_OK;
}

bool Ready() { return runtime.ready; }

void OnGather(Gatherer gatherer) {
    runtime.gatherer = gatherer;
    Poke();
}

void Poke() {
    if (runtime.task != nullptr) {
        xTaskAbortDelay(runtime.task);
    }
}

void ShowVerdict(bool allowed) {
    // Solid rather than a blink: this is a full stop, and something that is still
    // flashing when the operator looks up reads as a question rather than as an
    // answer. `led::SetFor` puts the ranking's colour back on its own.
    led::SetFor(allowed ? led::colour::kGreen : led::colour::kRed, led::Effect::kSolid,
                kVerdictFlashMs);
}

State Current() { return runtime.current; }

uint32_t Transitions() { return runtime.transitions; }

}  // namespace indicator
