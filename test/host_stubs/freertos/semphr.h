#pragma once

// Host-build double of the ESP-IDF FreeRTOS semaphore API used by the SIDP
// transports. The firmware resolves the real FreeRTOS headers; this file only
// exists so the host tests can link without ESP-IDF. The signatures mirror
// freertos/semphr.h, and the ESP-IDF additions are mirrored in
// freertos/idf_additions.h, which this header includes the same way the real
// FreeRTOS.h does. No ESP-IDF source tree is modified or shadowed in firmware.

#include <cstdint>
#include <mutex>

#include "freertos/FreeRTOS.h"

struct StaticSemaphore_t {
    std::mutex mutex;
};

using SemaphoreHandle_t = StaticSemaphore_t *;

inline int xSemaphoreTake(SemaphoreHandle_t handle, std::uint32_t)
{
    handle->mutex.lock();
    return 1;
}

inline int xSemaphoreGive(SemaphoreHandle_t handle)
{
    handle->mutex.unlock();
    return 1;
}

// ESP-IDF's FreeRTOS.h includes idf_additions.h implicitly for compatibility.
#include "freertos/idf_additions.h"
