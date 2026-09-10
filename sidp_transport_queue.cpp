#include "sidp_transport_queue.hpp"

#include "esp_heap_caps.h"
#include "esp_log.h"

namespace sidp
{
    bool packet_queue_transport::link_is_connected() const noexcept
    {
        return connected.load() && physical_link_open();
    }

    bool packet_queue_transport::is_open() const noexcept
    {
        return !dead.load() && link_is_connected();
    }

    bool packet_queue_transport::needs_disconnect() const noexcept
    {
        return !is_open();
    }

    void packet_queue_transport::drain(RingbufHandle_t ring) noexcept
    {
        if (ring == nullptr) return;
        std::size_t size = 0;
        while (auto *item = xRingbufferReceive(ring, &size, 0)) {
            vRingbufferReturnItem(ring, item);
        }
    }

    void packet_queue_transport::close_locked() noexcept
    {
        dead.store(true);
        epoch.fetch_add(1);
        drain(ring_buffer);
        drain(tx_ring);
        drain(log_ring);
        tx_pending = 0;
    }

    void packet_queue_transport::close_session() noexcept
    {
        const std::lock_guard lock(queue_mutex);
        close_locked();
    }

    void packet_queue_transport::link_changed(bool up) noexcept
    {
        const std::lock_guard lock(queue_mutex);
        connected.store(up);
        if (up) ++link_serial;
        close_locked();
    }

    esp_err_t packet_queue_transport::begin_session() noexcept
    {
        const std::lock_guard lock(queue_mutex);
        if (ring_buffer == nullptr || !dead.load() || !link_is_connected() ||
            link_serial == accepted_link_serial || tx_busy || rx_borrowed != 0) {
            return ESP_ERR_INVALID_STATE;
        }
        close_locked();
        reset_wire_buffers();
        accepted_link_serial = link_serial;
        dead.store(false);
        return ESP_OK;
    }

    esp_err_t packet_queue_transport::start_read(std::uint8_t **buf_out, std::size_t *len_out, std::uint32_t timeout_ms) noexcept
    {
        if (buf_out == nullptr || len_out == nullptr) return ESP_ERR_INVALID_ARG;
        *buf_out = nullptr;
        *len_out = 0;
        const TickType_t start = xTaskGetTickCount();
        const TickType_t limit = timeout_to_ticks(timeout_ms);
        for (;;) {
            {
                const std::lock_guard lock(queue_mutex);
                if (!is_open() || ring_buffer == nullptr) return ESP_ERR_INVALID_STATE;
                auto *packet = static_cast<std::uint8_t *>(xRingbufferReceive(ring_buffer, len_out, 0));
                if (packet != nullptr) {
                    ++rx_borrowed;
                    *buf_out = packet;
                    return ESP_OK;
                }
            }
            const TickType_t elapsed = xTaskGetTickCount() - start;
            if (timeout_ms != WAIT_FOREVER && elapsed >= limit) return ESP_ERR_TIMEOUT;
            const TickType_t slice = timeout_to_ticks(POLL_MS);
            vTaskDelay(timeout_ms == WAIT_FOREVER || limit - elapsed > slice ? slice : limit - elapsed);
        }
    }

    void packet_queue_transport::end_read(std::uint8_t *buf_return) noexcept
    {
        const std::lock_guard lock(queue_mutex);
        if (buf_return != nullptr && ring_buffer != nullptr) {
            vRingbufferReturnItem(ring_buffer, buf_return);
            --rx_borrowed;
        }
    }

    esp_err_t packet_queue_transport::create_queues() noexcept
    {
        ring_buffer = xRingbufferCreateWithCaps(QUEUE_SIZE, RINGBUF_TYPE_NOSPLIT, MALLOC_CAP_SPIRAM);
        tx_ring = xRingbufferCreateWithCaps(QUEUE_SIZE, RINGBUF_TYPE_NOSPLIT, MALLOC_CAP_SPIRAM);
        log_ring = xRingbufferCreateWithCaps(LOG_QUEUE_SIZE, RINGBUF_TYPE_NOSPLIT, MALLOC_CAP_SPIRAM);
        if (ring_buffer == nullptr || tx_ring == nullptr || log_ring == nullptr) {
            destroy_queues();
            return ESP_ERR_NO_MEM;
        }
        return ESP_OK;
    }

    void packet_queue_transport::destroy_queues() noexcept
    {
        for (auto *ring : {&ring_buffer, &tx_ring, &log_ring}) {
            if (*ring != nullptr) vRingbufferDeleteWithCaps(*ring);
            *ring = nullptr;
        }
    }

    void packet_queue_transport::deliver_packet(const std::uint8_t *data, std::size_t size, std::uint32_t received_epoch) noexcept
    {
        const std::span<const std::uint8_t> packet(data, size);
        if (!is_valid_message_size(size) || !crc32_hasher::verify_message_crc(packet)) return;
        const std::lock_guard lock(queue_mutex);
        if (!is_open() || received_epoch != epoch.load()) return;
        if (data[0] != PROTOCOL_VERSION || xRingbufferSend(ring_buffer, data, size, 0) != pdTRUE) {
            // Losing a request or receiving an unknown layout ends this session.
            close_locked();
        }
    }

    TickType_t packet_queue_transport::timeout_to_ticks(std::uint32_t timeout_ms) noexcept
    {
        if (timeout_ms == WAIT_FOREVER) return portMAX_DELAY;
        const std::uint64_t ticks = (static_cast<std::uint64_t>(timeout_ms) * configTICK_RATE_HZ + 999) / 1000;
        return ticks >= portMAX_DELAY ? portMAX_DELAY - 1 : static_cast<TickType_t>(ticks);
    }

    esp_err_t packet_queue_transport::enqueue(std::span<const std::uint8_t> message, bool log) noexcept
    {
        if (message.size() < sizeof(msg_header_t)) return ESP_ERR_INVALID_ARG;
        if (message.size() > (log ? MAX_LOG_FRAME_SIZE : MAX_FRAME_SIZE)) return ESP_ERR_INVALID_SIZE;
        if (log && reinterpret_cast<const msg_header_t *>(message.data())->kind != KIND_LOG_STREAM) {
            return ESP_ERR_INVALID_ARG;
        }
        const std::lock_guard lock(queue_mutex);
        if (!is_open() || tx_ring == nullptr) return ESP_ERR_INVALID_STATE;
        if (xRingbufferSend(log ? log_ring : tx_ring, message.data(), message.size(), 0) != pdTRUE) {
            tx_dropped.fetch_add(1);
            if (!log) close_locked();
            return ESP_ERR_NO_MEM;
        }
        ++tx_pending;
        if (tx_task_handle != nullptr) xTaskNotifyGive(tx_task_handle);
        return ESP_OK;
    }

    esp_err_t packet_queue_transport::write_message(std::span<const std::uint8_t> message) noexcept
    {
        return enqueue(message, false);
    }

    esp_err_t packet_queue_transport::write_message_log(std::span<const std::uint8_t> message) noexcept
    {
        return enqueue(message, true);
    }

    esp_err_t packet_queue_transport::flush_write(std::uint32_t timeout_ms) noexcept
    {
        const TickType_t start = xTaskGetTickCount();
        const TickType_t limit = timeout_to_ticks(timeout_ms);
        for (;;) {
            {
                const std::lock_guard lock(queue_mutex);
                if (!is_open()) return ESP_ERR_INVALID_STATE;
                if (tx_pending == 0 && !tx_busy) return ESP_OK;
            }
            const TickType_t elapsed = xTaskGetTickCount() - start;
            if (timeout_ms != WAIT_FOREVER && elapsed >= limit) return ESP_ERR_TIMEOUT;
            const TickType_t slice = timeout_to_ticks(POLL_MS);
            vTaskDelay(timeout_ms == WAIT_FOREVER || limit - elapsed > slice ? slice : limit - elapsed);
        }
    }

    std::uint32_t packet_queue_transport::tx_dropped_frames() const noexcept
    {
        return tx_dropped.load();
    }

    bool packet_queue_transport::tx_idle() noexcept
    {
        const std::lock_guard lock(queue_mutex);
        return !tx_busy;
    }

    bool packet_queue_transport::service_tx() noexcept
    {
        std::size_t size = 0;
        std::uint8_t *item = nullptr;
        RingbufHandle_t source = nullptr;
        bool log = false;
        {
            const std::lock_guard lock(queue_mutex);
            if (!is_open() || tx_busy) return false;
            source = tx_ring;
            item = static_cast<std::uint8_t *>(xRingbufferReceive(source, &size, 0));
            if (item == nullptr) {
                source = log_ring;
                log = true;
                item = static_cast<std::uint8_t *>(xRingbufferReceive(source, &size, 0));
            }
            if (item == nullptr) return false;
            --tx_pending;
            tx_busy = true;
        }
        // A new session cannot begin while this frame is in flight.
        const bool sent = deliver_tx_frame({item, size}, log);
        {
            const std::lock_guard lock(queue_mutex);
            vRingbufferReturnItem(source, item);
            if (!sent) {
                tx_dropped.fetch_add(1);
                close_locked();
                ESP_LOGE(TAG, "TX failed; session closed");
            }
            tx_busy = false;
        }
        return true;
    }

    esp_err_t packet_queue_transport::spawn_tx_task(const char *name) noexcept
    {
        if (tx_ring == nullptr || tx_task_handle != nullptr) return ESP_ERR_INVALID_STATE;
        return xTaskCreate(tx_task_trampoline, name, TX_TASK_STACK_SIZE, this, TX_TASK_PRIORITY,
                           &tx_task_handle) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
    }

    void packet_queue_transport::tx_task_trampoline(void *arg) noexcept
    {
        static_cast<packet_queue_transport *>(arg)->tx_task_loop();
    }

    void packet_queue_transport::tx_task_loop() noexcept
    {
        for (;;) {
            while (service_tx()) {}
            ulTaskNotifyTake(pdTRUE, timeout_to_ticks(POLL_MS));
        }
    }
}
