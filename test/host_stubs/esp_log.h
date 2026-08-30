#pragma once

// Host-build stub of ESP-IDF esp_log.h. Routes all levels to stderr in the
// same "E (tag): message" shape as the IDF console output. The firmware
// build resolves the real header from the log component.

#include <cstdio>

#define ESP_LOGE(tag, fmt, ...) fprintf(stderr, "E (%s): " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) fprintf(stderr, "W (%s): " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) fprintf(stderr, "I (%s): " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...) fprintf(stderr, "D (%s): " fmt "\n", tag, ##__VA_ARGS__)
