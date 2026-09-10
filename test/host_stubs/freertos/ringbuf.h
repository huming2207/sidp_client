#pragma once
#include "FreeRTOS.h"
#include <cassert>
#include <cstring>
#include <deque>
#include <unordered_map>
#include <vector>
struct host_ring {
    std::size_t capacity, used = 0;
    std::deque<void *> queued;
    std::unordered_map<void *, std::vector<std::uint8_t> *> items;
};
using RingbufHandle_t = host_ring *;
inline constexpr int RINGBUF_TYPE_NOSPLIT = 0;
inline bool host_fail_next_send = false;
inline RingbufHandle_t xRingbufferCreateWithCaps(std::size_t capacity, int, int) {
    auto *ring = new host_ring;
    ring->capacity = capacity;
    return ring;
}
inline BaseType_t xRingbufferSend(RingbufHandle_t ring, const void *data, std::size_t size, TickType_t wait) {
    assert(wait == 0); // A regression to waiting for queue space fails the test.
    if (host_fail_next_send) { host_fail_next_send = false; return 0; }
    if (size + 8 > ring->capacity / 2 || ring->used + size + 8 > ring->capacity) return 0;
    auto *item = new std::vector<std::uint8_t>(size);
    std::memcpy(item->data(), data, size);
    ring->items.emplace(item->data(), item);
    ring->queued.push_back(item->data());
    ring->used += size + 8;
    return pdTRUE;
}
inline void *xRingbufferReceive(RingbufHandle_t ring, std::size_t *size, TickType_t wait) {
    assert(wait == 0);
    if (ring->queued.empty()) { *size = 0; return nullptr; }
    void *item = ring->queued.front();
    ring->queued.pop_front();
    *size = ring->items.at(item)->size();
    return item;
}
inline void vRingbufferReturnItem(RingbufHandle_t ring, void *data) {
    auto *item = ring->items.at(data);
    ring->used -= item->size() + 8;
    ring->items.erase(data);
    delete item;
}
inline void vRingbufferDeleteWithCaps(RingbufHandle_t ring) {
    for (auto &[_, item] : ring->items) delete item;
    delete ring;
}
