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
     * @brief transport_intf base with a decoded-packet receive queue and an
     *        outbound frame queue driven by the transport's own TX task.
     *
     * Concrete transports decode their wire format (SLIP, WebSocket) and call
     * deliver_packet() for every complete candidate; this base validates the
     * SIDP size and CRC32 and queues accepted packets into one per-instance
     * NOSPLIT ring buffer. Outbound frames enqueued through write_message()
     * wait in a second per-instance NOSPLIT ring buffer; the TX task created
     * by spawn_tx_task() dequeues them and hands each one to the concrete
     * transport via deliver_tx_frame().
     *
     * Each instance owns its own queues; transports that run concurrently
     * never share a buffer.
     *
     * RX: one producer (the transport receive callback) and one consumer (the
     * start_read() caller) may operate concurrently. TX: any number of
     * producers (write_message / write_message_expendable) and exactly one
     * consumer (the TX task).
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

        /** @copydoc transport_intf::write_message */
        [[nodiscard]] esp_err_t write_message(std::span<const std::uint8_t> message) noexcept final;

        /** @copydoc transport_intf::write_message_expendable */
        [[nodiscard]] esp_err_t write_message_log(std::span<const std::uint8_t> message) noexcept final;

        /** @copydoc transport_intf::flush_write */
        [[nodiscard]] esp_err_t flush_write(std::uint32_t timeout_ms) noexcept final;

        /** @copydoc transport_intf::tx_dropped_frames */
        [[nodiscard]] std::uint32_t tx_dropped_frames() const noexcept final;

    protected:
        packet_queue_transport() noexcept = default;
        ~packet_queue_transport() override = default;

        /**
         * @brief Allocates the receive and transmit queues in PSRAM.
         * @return ESP_OK on success, ESP_ERR_NO_MEM if a queue cannot be allocated.
         */
        [[nodiscard]] esp_err_t create_queues() noexcept;

        /** @brief Frees both queues. Idempotent. */
        void destroy_queues() noexcept;

        /**
         * @brief Creates and starts the TX task. Call once from init().
         * @return ESP_OK on success, ESP_ERR_NO_MEM if the task cannot be created.
         */
        [[nodiscard]] esp_err_t spawn_tx_task(const char *name) noexcept;

        /**
         * @brief Discards every queued outbound frame.
         *
         * Called by concrete transports when the link goes down: stale frames
         * from a detached session must never reach a re-attached host. Safe
         * against the TX task: frames it already dequeued are dropped by
         * deliver_tx_frame() returning false.
         */
        void drain_tx() noexcept;

        /**
         * @brief Blocking send primitive for one frame (TX task context).
         *
         * Implementations encode/transmit the frame and return false when the
         * frame could not reach the wire (link down, send error); the base
         * drops the frame and logs.
         */
        [[nodiscard]] virtual bool deliver_tx_frame(std::span<const std::uint8_t> frame) noexcept = 0;

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
        /** @brief Capacity of each ring buffer (RX and TX) in PSRAM. */
        static constexpr std::size_t QUEUE_SIZE = 131072;
        static constexpr std::size_t TX_TASK_STACK_SIZE = 4096;
        static constexpr UBaseType_t TX_TASK_PRIORITY = 3;
        /** Poll interval for flush_write(); must be at least one FreeRTOS tick. */
        static constexpr std::uint32_t TX_FLUSH_POLL_MS = 10;
        static constexpr char TAG[] = "sidp_tq";

        static void tx_task_trampoline(void *arg) noexcept;
        void tx_task_loop() noexcept;

        RingbufHandle_t ring_buffer = nullptr;
        std::atomic<bool> rx_overflow{false};

        RingbufHandle_t tx_ring = nullptr;
        TaskHandle_t tx_task_handle = nullptr;
        std::atomic<int> tx_queued{0};
        std::atomic<bool> tx_busy{false};
        std::atomic<std::uint32_t> tx_dropped{0};
    };
}
