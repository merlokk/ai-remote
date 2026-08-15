#pragma once

#include "FreeRTOS.h"

// Adds to a running total instead of sleeping. The drivers wait out chip
// settling times (the ES8311's reset, the QMI8658's power-up), and a test
// suite that actually slept for them would take seconds to say nothing.
void vTaskDelay(TickType_t ticks);
