#include "sidp_transport_queue.hpp"

#include "esp_heap_caps.h"
#include "esp_log.h"

namespace sidp
{

    esp_err_t packet_queue_transport::start_read(std::uint8_t **buf_out, std::size_t *len_out, std::uint32_t timeout_ms) noexcept
    {
        if (buf_out == nullptr || len_out == nullptr) {
            return ESP_ERR_INVALID_ARG;
        }

        *buf_out = nullptr;
        *len_out = 0;
        if (ring_buffer == nullptr) {
            return ESP_ERR_INVALID_STATE;
        }
        if (rx_overflow.exchange(false, std::memory_order_relaxed)) {
            return ESP_ERR_NO_MEM;
        }

        std::size_t packet_size = 0;
        auto *packet = static_cast<std::uint8_t *>(xRingbufferReceive(ring_buffer, &packet_size, timeout_to_ticks(timeout_ms)));
        if (packet == nullptr) {
            return is_open() ? ESP_ERR_TIMEOUT : ESP_ERR_INVALID_STATE;
        }

        *buf_out = packet;
        *len_out = packet_size;
        return 0;
    }

    void packet_queue_transport::end_read(std::uint8_t *buf_return) noexcept
    {
        if (buf_return != nullptr && ring_buffer != nullptr) {
            vRingbufferReturnItem(ring_buffer, buf_return);
        }
    }

    esp_err_t packet_queue_transport::create_packet_queue() noexcept
    {
        ring_buffer = xRingbufferCreateWithCaps(RX_PACKET_RING_BUFFER_SIZE, RINGBUF_TYPE_NOSPLIT, MALLOC_CAP_SPIRAM);
        if (ring_buffer == nullptr) {
            return ESP_ERR_NO_MEM;
        }
        return ESP_OK;
    }

    void packet_queue_transport::destroy_packet_queue() noexcept
    {
        if (ring_buffer != nullptr) {
            vRingbufferDeleteWithCaps(ring_buffer);
            ring_buffer = nullptr;
        }
    }

    void packet_queue_transport::deliver_packet(const std::uint8_t *data, std::size_t size) noexcept
    {
        const std::span<const std::uint8_t> packet(data, size);
        if (!is_valid_message_size(size) || !crc32_hasher::verify_message_crc(packet)) {
            ESP_LOGE(TAG, "deliver_packet: rx frame dropped: bad size or CRC (%u bytes)", static_cast<unsigned>(size));
            return;
        }

        if (xRingbufferSend(ring_buffer, data, size, 0) != pdTRUE) {
            rx_overflow.store(true, std::memory_order_relaxed);
            ESP_LOGE(TAG, "deliver_packet: rx queue full: frame dropped (%u bytes)", static_cast<unsigned>(size));
        }
    }

    TickType_t packet_queue_transport::timeout_to_ticks(std::uint32_t timeout_ms) noexcept
    {
        if (timeout_ms == WAIT_FOREVER) {
            return portMAX_DELAY;
        }
        if (timeout_ms == 0) {
            return 0;
        }

        const TickType_t ticks = pdMS_TO_TICKS(timeout_ms);
        return ticks == 0 ? 1 : ticks;
    }

    // ---- TX frame queue (MPSC) --------------------------------------------------

    esp_err_t sidp_tx_queue::init() noexcept
    {
        if (ring != nullptr) {
            ESP_LOGE(TAG, "init: already initialized");
            return ESP_ERR_INVALID_STATE;
        }

        ring = xRingbufferCreateWithCaps(TX_CAPACITY, RINGBUF_TYPE_NOSPLIT, MALLOC_CAP_SPIRAM);
        if (ring == nullptr) {
            ESP_LOGE(TAG, "init: ringbuffer allocation failed");
            return ESP_ERR_NO_MEM;
        }

        ESP_LOGI(TAG, "init: tx queue ready (%u KiB, PSRAM)", static_cast<unsigned>(TX_CAPACITY / 1024u));
        return ESP_OK;
    }

    bool sidp_tx_queue::enqueue_blocking(std::span<const std::uint8_t> frame, TickType_t timeout_ticks) noexcept
    {
        if (ring == nullptr || frame.empty() || frame.size() > TX_CAPACITY) {
            return false;
        }
        return xRingbufferSend(ring, frame.data(), frame.size(), timeout_ticks) == pdTRUE;
    }

    bool sidp_tx_queue::enqueue_drop(std::span<const std::uint8_t> frame) noexcept
    {
        if (ring == nullptr || frame.empty() || frame.size() > TX_CAPACITY) {
            return false;
        }
        if (xRingbufferSend(ring, frame.data(), frame.size(), 0) != pdTRUE) {
            // Counted, not logged: at log-stream rates this could flood the
            // console. The owner task can report dropped_frames() periodically.
            dropped.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        return true;
    }

    std::span<const std::uint8_t> sidp_tx_queue::receive(TickType_t timeout_ticks) noexcept
    {
        if (ring == nullptr) {
            return {};
        }
        std::size_t size = 0;
        auto *item = static_cast<std::uint8_t *>(xRingbufferReceive(ring, &size, timeout_ticks));
        if (item == nullptr) {
            return {};
        }
        return {item, size};
    }

    void sidp_tx_queue::return_item(std::uint8_t *item) noexcept
    {
        if (ring != nullptr && item != nullptr) {
            vRingbufferReturnItem(ring, item);
        }
    }

    void sidp_tx_queue::drain() noexcept
    {
        if (ring == nullptr) {
            return;
        }

        std::size_t drained = 0;
        while (true) {
            std::size_t size = 0;
            auto *item = static_cast<std::uint8_t *>(xRingbufferReceive(ring, &size, 0));
            if (item == nullptr) {
                break;
            }
            vRingbufferReturnItem(ring, item);
            ++drained;
        }

        if (drained != 0) {
            ESP_LOGI(TAG, "drain: discarded %u queued frames", static_cast<unsigned>(drained));
        }
    }

}