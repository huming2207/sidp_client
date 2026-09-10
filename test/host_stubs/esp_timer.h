#pragma once
#include "freertos/task.h"
inline std::int64_t esp_timer_get_time() { return static_cast<std::int64_t>(host_ticks.load()) * 10000; }
