#pragma once
#include "FreeRTOS.h"
typedef struct { int taken; } StaticSemaphore_t;
typedef StaticSemaphore_t *SemaphoreHandle_t;
static inline SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *storage)
{
    storage->taken = 0;
    return storage;
}
static inline int xSemaphoreTake(SemaphoreHandle_t mutex, uint32_t ticks)
{
    (void)ticks;
    if (mutex->taken) return 0;
    mutex->taken = 1;
    return pdTRUE;
}
static inline void xSemaphoreGive(SemaphoreHandle_t mutex) { mutex->taken = 0; }
