#pragma once

#include "FreeRTOS.h"

namespace fake {
// POD, because `Bus` holds a `StaticSemaphore_t` by value as a member.
struct Semaphore {
    bool created;
    bool taken;
};
}  // namespace fake

typedef fake::Semaphore StaticSemaphore_t;
typedef fake::Semaphore *SemaphoreHandle_t;

SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *storage);

// **Never blocks, and records what it was asked for.** A fake that slept could
// not distinguish "waited 20 ms and gave up" from "waited forever" — and that
// distinction is exactly what §10.14.3's rule is about, so it is the one thing
// this has to make visible.
BaseType_t xSemaphoreTake(SemaphoreHandle_t handle, TickType_t ticks);
BaseType_t xSemaphoreGive(SemaphoreHandle_t handle);
