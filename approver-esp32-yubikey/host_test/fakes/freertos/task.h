#pragma once

#include "FreeRTOS.h"

// Adds to a running total instead of sleeping. The one driver in the host tier
// that waits is `buttons`, which polls a pin every `kPollIntervalMs` and can be
// asked whether one was held for five seconds — and a test suite that actually
// slept for that would take five seconds to say nothing.
void vTaskDelay(TickType_t ticks);

// Starts nothing and says so by returning a non-null handle: `Init` treats a null
// as "the deny button will not work" and logs it, which is not what the host tier
// is testing.
TaskHandle_t xTaskCreateStatic(void (*fn)(void *), const char *name, uint32_t depth, void *arg,
                               UBaseType_t priority, StackType_t *stack, StaticTask_t *tcb);
