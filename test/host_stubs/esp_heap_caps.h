#pragma once

// Host-build stub of the ESP-IDF esp_heap_caps.h subset used by the SIDP
// component. Allocation capability flags are ignored on the host. The
// firmware build resolves the real header from the IDF.

#include <cstddef>
#include <cstdint>
#include <cstdlib>

#define MALLOC_CAP_SPIRAM (1 << 0)
#define MALLOC_CAP_INTERNAL (1 << 1)
#define MALLOC_CAP_DEFAULT (1 << 3)

inline void *heap_caps_calloc(std::size_t n, std::size_t size, std::uint32_t /*caps */)
{
    return std::calloc(n, size);
}

inline void *heap_caps_malloc(std::size_t size, std::uint32_t /*caps */)
{
    return std::malloc(size);
}

inline void heap_caps_free(void *ptr)
{
    std::free(ptr);
}
