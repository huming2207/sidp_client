#include "sidp_transport_queue.hpp"

namespace sidp
{

    int packet_queue_transport::start_read(std::uint8_t **buf_out, std::size_t *len_out, std::uint32_t timeout_ms) noexcept
    {
        if (buf_out == nullptr || len_out == nullptr) {
            return -EINVAL;
        }

        *buf_out = nullptr;
        *len_out = 0;
        if (ring_buffer == nullptr) {
            return -ENOTCONN;
        }
        if (rx_overflow.exchange(false, std::memory_order_relaxed)) {
            return -ENOBUFS;
        }

        std::size_t packet_size = 0;
        auto *packet = static_cast<std::uint8_t *>(xRingbufferReceive(ring_buffer, &packet_size, timeout_to_ticks(timeout_ms)));
        if (packet == nullptr) {
            return is_open() ? -ETIMEDOUT : -ENOTCONN;
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

    int packet_queue_transport::create_packet_queue() noexcept
    {
        ring_buffer = xRingbufferCreateWithCaps(RX_PACKET_RING_BUFFER_SIZE, RINGBUF_TYPE_NOSPLIT, MALLOC_CAP_SPIRAM);
        if (ring_buffer == nullptr) {
            return -ENOMEM;
        }
        return 0;
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
            return;
        }

        if (xRingbufferSend(ring_buffer, data, size, 0) != pdTRUE) {
            rx_overflow.store(true, std::memory_order_relaxed);
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

}
