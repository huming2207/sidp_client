#pragma once

// Host-build double of the ESP-IDF FreeRTOS additions in
// freertos/idf_additions.h. Only the two functions used by this component are
// provided, and their signatures are copied verbatim from the real header:
//
//   static inline SemaphoreHandle_t xSemaphoreCreateMutexWithCaps(UBaseType_t uxMemoryCaps);
//   void vSemaphoreDeleteWithCaps(SemaphoreHandle_t xSemaphore);
//
// On the host the "capability" argument is ignored and the mutex is a heap
// object; on the ESP32 the real implementation allocates it from PSRAM.

#include "freertos/semphr.h"

// Mirrors freertos/idf_additions.h:460.
inline SemaphoreHandle_t xSemaphoreCreateMutexWithCaps(UBaseType_t /* uxMemoryCaps */)
{
    return new StaticSemaphore_t;
}

// Mirrors freertos/idf_additions.h:497.
inline void vSemaphoreDeleteWithCaps(SemaphoreHandle_t handle)
{
    delete handle;
}
