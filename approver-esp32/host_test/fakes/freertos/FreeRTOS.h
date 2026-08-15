#pragma once

#include <cstdint>

typedef uint32_t TickType_t;
typedef int BaseType_t;

#define pdTRUE 1
#define pdFALSE 0

// **A 1 kHz tick, so a tick is a millisecond and a test can assert the number
// it passed in.** The firmware sets `CONFIG_FREERTOS_HZ=1000` (sdkconfig.defaults,
// for LVGL), so this is not a convenient fiction — it is the same rate.
#define configTICK_RATE_HZ 1000
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))

#define portMAX_DELAY ((TickType_t)0xffffffffUL)
