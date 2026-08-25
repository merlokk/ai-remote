#pragma once

#include "FreeRTOS.h"

// Adds to a running total instead of sleeping. The one driver in the host tier
// that waits is `buttons`, which polls a pin every `kPollIntervalMs` and can be
// asked whether one was held for five seconds — and a test suite that actually
// slept for that would take five seconds to say nothing.
void vTaskDelay(TickType_t ticks);
