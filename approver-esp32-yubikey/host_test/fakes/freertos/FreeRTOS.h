#pragma once

#include <cstdint>

typedef uint32_t TickType_t;
typedef int BaseType_t;

#define pdTRUE 1
#define pdFALSE 0

// **A 1 kHz tick, so a tick is a millisecond and a test can assert the number
// it passed in.** The firmware sets `CONFIG_FREERTOS_HZ=1000`
// (sdkconfig.defaults says why), so this is not a convenient fiction — it is the
// same rate.
#define configTICK_RATE_HZ 1000
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))

#define portMAX_DELAY ((TickType_t)0xffffffffUL)

// Enough of a task to let `buttons::Buttons::Init` compile and start nothing.
// **The host tier drives the debounce and the latch directly**, which is the point
// of them being a pure class (`Debounce`) — a fake scheduler that actually ran the
// poller would make every test in that suite depend on a thread's timing instead of
// on the numbers it passes in.
typedef unsigned long UBaseType_t;
typedef void *TaskHandle_t;
typedef struct {
    unsigned long dummy;
} StaticTask_t;
typedef unsigned long StackType_t;
