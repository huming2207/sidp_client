#pragma once

// Host-build stub of the ESP-IDF/FreeRTOS static semaphore API used by the
// SIDP transports. The firmware resolves the real header from FreeRTOS; this
// stub backs the static mutex with a std::mutex so host tests exercise the same
// take/give contract without any of the real allocator behavior.

#include <cstdint>
#include <mutex>

struct StaticSemaphore_t {
    std::mutex mutex;
};

using SemaphoreHandle_t = StaticSemaphore_t *;

inline SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *storage)
{
    return storage;
}

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
