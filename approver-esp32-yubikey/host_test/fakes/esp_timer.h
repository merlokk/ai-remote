#pragma once

#include <cstdint>

// Microseconds since boot, off the fake clock in `fake_platform.h`. It moves
// only when a test advances it or a `vTaskDelay` does — see the note there for
// why a free-running clock would be worse than a stopped one.
int64_t esp_timer_get_time(void);
