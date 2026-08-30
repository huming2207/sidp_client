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

    esp_err_t packet_queue_transport::create_queues() noexcept
    {
        ring_buffer = xRingbufferCreateWithCaps(QUEUE_SIZE, RINGBUF_TYPE_NOSPLIT, MALLOC_CAP_SPIRAM);
        if (ring_buffer == nullptr) {
            ESP_LOGE(TAG, "create_queues: rx ringbuffer allocation failed");
            return ESP_ERR_NO_MEM;
        }
        tx_ring = xRingbufferCreateWithCaps(QUEUE_SIZE, RINGBUF_TYPE_NOSPLIT, MALLOC_CAP_SPIRAM);
        if (tx_ring == nullptr) {
            ESP_LOGE(TAG, "create_queues: tx ringbuffer allocation failed");
            vRingbufferDeleteWithCaps(ring_buffer);
            ring_buffer = nullptr;
            return ESP_ERR_NO_MEM;
        }
        return ESP_OK;
    }

    void packet_queue_transport::destroy_queues() noexcept
    {
        if (tx_ring != nullptr) {
            vRingbufferDeleteWithCaps(tx_ring);
            tx_ring = nullptr;
        }
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

    // ---- TX path ---------------------------------------------------------------------------

    esp_err_t packet_queue_transport::write_message(std::span<const std::uint8_t> message) noexcept
    {
        if (message.size() < sizeof(msg_header_t)) {
            return ESP_ERR_INVALID_ARG;
        }
        if (message.size() > MAX_FRAME_SIZE) {
            return ESP_ERR_INVALID_SIZE;
        }
        if (tx_ring == nullptr) {
            return ESP_ERR_INVALID_STATE;
        }
        if (xRingbufferSend(tx_ring, message.data(), message.size(),
                            timeout_to_ticks(TX_ENQUEUE_TIMEOUT_MS)) != pdTRUE) {
            ESP_LOGE(TAG, "write_message: tx queue full for %u ms, frame lost (%u bytes)",
                     TX_ENQUEUE_TIMEOUT_MS, static_cast<unsigned>(message.size()));
            return ESP_ERR_TIMEOUT;
        }
        tx_queued.fetch_add(1, std::memory_order_relaxed);
        return ESP_OK;
    }

    esp_err_t packet_queue_transport::write_message_log(std::span<const std::uint8_t> message) noexcept
    {
        if (message.size() < sizeof(msg_header_t)) {
            return ESP_ERR_INVALID_ARG;
        }
        if (message.size() > MAX_FRAME_SIZE) {
            return ESP_ERR_INVALID_SIZE;
        }
        if (tx_ring == nullptr) {
            return ESP_ERR_INVALID_STATE;
        }
        if (xRingbufferSend(tx_ring, message.data(), message.size(), 0) != pdTRUE) {
            // Counted, not logged: at log-stream rates this could flood the
            // console. The owner can report tx_dropped_frames() periodically.
            tx_dropped.fetch_add(1, std::memory_order_relaxed);
            return ESP_ERR_NO_MEM;
        }
        tx_queued.fetch_add(1, std::memory_order_relaxed);
        return ESP_OK;
    }

    esp_err_t packet_queue_transport::flush_write(std::uint32_t timeout_ms) noexcept
    {
        if (tx_ring == nullptr) {
            return ESP_ERR_INVALID_STATE;
        }

        const TickType_t start = xTaskGetTickCount();
        const TickType_t limit = timeout_ms == WAIT_FOREVER ? portMAX_DELAY : timeout_to_ticks(timeout_ms);
        while (tx_queued.load(std::memory_order_relaxed) != 0 || tx_busy.load(std::memory_order_relaxed)) {
            if ((xTaskGetTickCount() - start) >= limit) {
                ESP_LOGE(TAG, "flush_write: tx queue not drained within %u ms", timeout_ms);
                return ESP_ERR_TIMEOUT;
            }
            vTaskDelay(pdMS_TO_TICKS(TX_FLUSH_POLL_MS));
        }
        return ESP_OK;
    }

    std::uint32_t packet_queue_transport::tx_dropped_frames() const noexcept
    {
        return tx_dropped.load(std::memory_order_relaxed);
    }

    esp_err_t packet_queue_transport::spawn_tx_task(const char *name) noexcept
    {
        if (tx_ring == nullptr) {
            ESP_LOGE(TAG, "spawn_tx_task: queues not created");
            return ESP_ERR_INVALID_STATE;
        }
        if (tx_task_handle != nullptr) {
            ESP_LOGE(TAG, "spawn_tx_task: already spawned");
            return ESP_ERR_INVALID_STATE;
        }
        if (xTaskCreate(tx_task_trampoline, name, TX_TASK_STACK_SIZE, this, TX_TASK_PRIORITY, &tx_task_handle) != pdPASS) {
            ESP_LOGE(TAG, "spawn_tx_task: task creation failed");
            return ESP_ERR_NO_MEM;
        }
        return ESP_OK;
    }

    void packet_queue_transport::tx_task_trampoline(void *arg) noexcept
    {
        static_cast<packet_queue_transport *>(arg)->tx_task_loop();
    }

    void packet_queue_transport::tx_task_loop() noexcept
    {
        while (true) {
            std::size_t size = 0;
            auto *item = static_cast<std::uint8_t *>(xRingbufferReceive(tx_ring, &size, portMAX_DELAY));
            if (item == nullptr) {
                continue;
            }
            // Mark busy before decrementing: a concurrent flush_write() seeing
            // zero queued frames must not miss the frame in flight.
            tx_busy.store(true, std::memory_order_relaxed);
            tx_queued.fetch_sub(1, std::memory_order_relaxed);
            const bool sent = deliver_tx_frame(std::span<const std::uint8_t>(item, size));
            tx_busy.store(false, std::memory_order_relaxed);
            vRingbufferReturnItem(tx_ring, item);
            if (!sent) {
                ESP_LOGE(TAG, "tx task: frame dropped (%u bytes, link down or send failure)",
                         static_cast<unsigned>(size));
            }
        }
    }

    void packet_queue_transport::drain_tx() noexcept
    {
        if (tx_ring == nullptr) {
            return;
        }

        std::size_t drained = 0;
        while (true) {
            std::size_t size = 0;
            auto *item = static_cast<std::uint8_t *>(xRingbufferReceive(tx_ring, &size, 0));
            if (item == nullptr) {
                break;
            }
            vRingbufferReturnItem(tx_ring, item);
            tx_queued.fetch_sub(1, std::memory_order_relaxed);
            ++drained;
        }

        if (drained != 0) {
            ESP_LOGI(TAG, "drain_tx: discarded %u queued frames", static_cast<unsigned>(drained));
        }
    }

}
