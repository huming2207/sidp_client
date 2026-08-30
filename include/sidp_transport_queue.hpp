#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"

#include "sidp_transport.hpp"

namespace sidp
{

    /**
     * @brief transport_intf base with a decoded-packet receive queue.
     *
     * Concrete transports decode their wire format (SLIP, WebSocket) and call
     * deliver_packet() for every complete candidate. This base validates the
     * SIDP size and CRC32, queues accepted packets into one per-instance
     * NOSPLIT ring buffer, and tracks overflow in one atomic flag. Each
     * instance owns its own queue; transports that run concurrently never
     * share a buffer.
     *
     * One producer (the transport receive callback) and one consumer (the
     * start_read() caller) may operate concurrently.
     */
    class packet_queue_transport : public transport_intf
    {
    public:
        packet_queue_transport(const packet_queue_transport &) = delete;
        packet_queue_transport &operator=(const packet_queue_transport &) = delete;

        /** @copydoc transport_intf::start_read */
        [[nodiscard]] int start_read(std::uint8_t **buf_out, std::size_t *len_out, std::uint32_t timeout_ms) noexcept final;

        /** @copydoc transport_intf::end_read */
        void end_read(std::uint8_t *buf_return) noexcept final;

    protected:
        packet_queue_transport() noexcept = default;
        ~packet_queue_transport() override = default;

        /**
         * @brief Allocates the receive queue in PSRAM.
         * @return 0 on success, -ENOMEM if the queue cannot be allocated.
         */
        [[nodiscard]] int create_packet_queue() noexcept;

        /** @brief Frees the receive queue. Idempotent. */
        void destroy_packet_queue() noexcept;

        /**
         * @brief Validates and queues one complete decoded packet.
         *
         * Packets failing the SIDP size or CRC check are dropped. When the
         * queue is full the packet is dropped and the overflow flag is set;
         * the next start_read() reports @c -ENOBUFS once.
         *
         * @param data Decoded packet bytes.
         * @param size Packet size in bytes.
         */
        void deliver_packet(const std::uint8_t *data, std::size_t size) noexcept;

        /** @brief Converts milliseconds to FreeRTOS ticks without rounding down. */
        [[nodiscard]] static TickType_t timeout_to_ticks(std::uint32_t timeout_ms) noexcept;

    private:
        /** @brief Capacity of the decoded-packet ring buffer in PSRAM. */
        static constexpr std::size_t RX_PACKET_RING_BUFFER_SIZE = 131072;

        RingbufHandle_t ring_buffer = nullptr;
        std::atomic<bool> rx_overflow{false};
    };

}
