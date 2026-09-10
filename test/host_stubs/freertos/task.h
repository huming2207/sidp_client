#pragma once
#include "FreeRTOS.h"
#include <thread>
inline TickType_t xTaskGetTickCount() { return host_ticks.load(); }
inline void vTaskDelay(TickType_t ticks) { host_ticks.fetch_add(ticks); std::this_thread::yield(); }
// Host tests drive service_tx() explicitly; no background firmware task starts.
inline BaseType_t xTaskCreate(void (*)(void *), const char *, unsigned, void *, UBaseType_t, TaskHandle_t *out) {
    *out = reinterpret_cast<void *>(1);
    return pdPASS;
}
inline void xTaskNotifyGive(TaskHandle_t) {}
inline unsigned ulTaskNotifyTake(int, TickType_t ticks) { vTaskDelay(ticks); return 0; }
