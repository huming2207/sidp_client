#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>

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
        [[nodiscard]] esp_err_t start_read(std::uint8_t **buf_out, std::size_t *len_out, std::uint32_t timeout_ms) noexcept final;

        /** @copydoc transport_intf::end_read */
        void end_read(std::uint8_t *buf_return) noexcept final;

    protected:
        packet_queue_transport() noexcept = default;
        ~packet_queue_transport() override = default;

        /**
         * @brief Allocates the receive queue in PSRAM.
         * @return ESP_OK on success, ESP_ERR_NO_MEM if the queue cannot be allocated.
         */
        [[nodiscard]] esp_err_t create_packet_queue() noexcept;

        /** @brief Frees the receive queue. Idempotent. */
        void destroy_packet_queue() noexcept;

        /**
         * @brief Validates and queues one complete decoded packet.
         *
         * Packets failing the SIDP size or CRC check are dropped. When the
         * queue is full the packet is dropped and the overflow flag is set;
         * the next start_read() reports @c ESP_ERR_NO_MEM once.
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
        static constexpr char TAG[] = "sidp_tq";

        RingbufHandle_t ring_buffer = nullptr;
        std::atomic<bool> rx_overflow{false};
    };

    /**
     * @brief MPSC frame queue between producers and the single TX task.
     *
     * Producers assemble complete SIDP frames and enqueue them; the TX task
     * dequeues one frame at a time and serializes it onto the transport
     * (write_message + flush_write). FIFO order preserves the protocol's
     * causal ordering (a response is always enqueued before the STOPPED /
     * TARGET_LOST event it causes).
     *
     * Backpressure policy is per producer:
     *  - enqueue_blocking(): session responses and events must not be lost,
     *    so the producer waits for queue space.
     *  - enqueue_drop(): log frames are expendable; when the queue is full
     *    the frame is dropped and counted.
     *
     * Not thread-safe against init()/drain(): those belong to the owner
     * task. enqueue_* / receive / return_item are safe from multiple tasks.
     */
    class sidp_tx_queue
    {
    public:
        static constexpr std::size_t TX_CAPACITY = 128u * 1024u;

        /** @brief Allocates the ring buffer in PSRAM. Owner task only. */
        [[nodiscard]] esp_err_t init() noexcept;

        /**
         * @brief Enqueues one complete frame, waiting up to timeout_ticks.
         * @return false when the queue stayed full for the whole timeout
         *         (or the frame is empty/oversized); the frame is lost.
         */
        [[nodiscard]] bool enqueue_blocking(std::span<const std::uint8_t> frame, TickType_t timeout_ticks) noexcept;

        /**
         * @brief Enqueues one complete frame without ever blocking.
         * @return false when the queue is full; the frame is dropped and
         *         counted in dropped_frames().
         */
        [[nodiscard]] bool enqueue_drop(std::span<const std::uint8_t> frame) noexcept;

        /** @brief Dequeues one frame; empty span on timeout. TX task only. */
        [[nodiscard]] std::span<const std::uint8_t> receive(TickType_t timeout_ticks) noexcept;

        /** @brief Releases a frame returned by receive(). TX task only. */
        void return_item(std::uint8_t *item) noexcept;

        /**
         * @brief Discards every queued frame. Owner task only, while the TX
         *        task is not forwarding (link down): stale frames from a
         *        detached session must never reach a re-attached host.
         */
        void drain() noexcept;

        /** @brief Number of frames dropped by enqueue_drop() so far. */
        [[nodiscard]] std::uint32_t dropped_frames() const noexcept
        {
            return dropped.load(std::memory_order_relaxed);
        }

        static constexpr char TAG[] = "sidp_txq";

    private:
        RingbufHandle_t ring = nullptr;
        std::atomic<std::uint32_t> dropped{0};
    };

}
