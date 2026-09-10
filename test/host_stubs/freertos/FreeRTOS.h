#pragma once
#include <atomic>
#include <cstdint>
using TickType_t = std::uint32_t;
using UBaseType_t = unsigned;
using BaseType_t = int;
using TaskHandle_t = void *;
inline constexpr TickType_t portMAX_DELAY = UINT32_MAX;
inline constexpr unsigned configTICK_RATE_HZ = 100;
inline constexpr int pdTRUE = 1, pdPASS = 1;
#define pdMS_TO_TICKS(ms) ((ms) / 10)
inline std::atomic<TickType_t> host_ticks{0};
