#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>

#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sidp_transport.hpp"

namespace sidp
{
    /**
     * Complete-packet queues shared by CDC and WebSocket.
     * A physical connection is not automatically a SIDP session. The owner calls
     * begin_session() after cleaning up its previous target session. Link loss,
     * RX overflow and TX failure latch the transport closed until then.
     *
     * Queue operations share a short mutex; no wire I/O or wait for queue space
     * happens under it. Control frames have their own FIFO ahead of bounded logs.
     */
    class packet_queue_transport : public transport_intf
    {
    public:
        packet_queue_transport(const packet_queue_transport &) = delete;
        packet_queue_transport &operator=(const packet_queue_transport &) = delete;

        /**
         * Accepts the current physical connection as a fresh SIDP session.
         * Call only after successful target cleanup and stopping old producers.
         * Returns INVALID_STATE while a TX frame or borrowed RX buffer remains,
         * or while the physical link is down. Retry from the owner task later.
         */
        [[nodiscard]] esp_err_t begin_session() noexcept final;
        /** Closes the session, discards queued work and latches needs_disconnect(). */
        void close_session() noexcept final;
        [[nodiscard]] bool needs_disconnect() const noexcept final;
        [[nodiscard]] bool is_open() const noexcept final;
        [[nodiscard]] bool link_is_connected() const noexcept final;

        [[nodiscard]] esp_err_t start_read(std::uint8_t **buf_out, std::size_t *len_out, std::uint32_t timeout_ms) noexcept final;
        void end_read(std::uint8_t *buf_return) noexcept final;
        [[nodiscard]] esp_err_t write_message(std::span<const std::uint8_t> message) noexcept final;
        [[nodiscard]] esp_err_t write_message_log(std::span<const std::uint8_t> message) noexcept final;
        [[nodiscard]] esp_err_t flush_write(std::uint32_t timeout_ms) noexcept final;
        [[nodiscard]] std::uint32_t tx_dropped_frames() const noexcept final;

    protected:
        packet_queue_transport() noexcept = default;
        ~packet_queue_transport() override = default;
        [[nodiscard]] esp_err_t create_queues() noexcept;
        void destroy_queues() noexcept;
        [[nodiscard]] esp_err_t spawn_tx_task(const char *name) noexcept;

        /** Called by link callbacks on both connect and disconnect boundaries. */
        void link_changed(bool connected) noexcept;
        [[nodiscard]] virtual bool physical_link_open() const noexcept = 0;
        /** Called with no TX in flight and the session closed, before acceptance. */
        virtual void reset_wire_buffers() noexcept {}
        [[nodiscard]] virtual bool deliver_tx_frame(std::span<const std::uint8_t> frame, bool log) noexcept = 0;

        /**
         * RX callbacks sample this token before reading/assembling bytes and pass
         * it to deliver_packet(). Work started before a boundary is discarded.
         */
        [[nodiscard]] std::uint32_t receive_epoch() const noexcept { return epoch.load(); }
        void deliver_packet(const std::uint8_t *data, std::size_t size, std::uint32_t received_epoch) noexcept;
        [[nodiscard]] static TickType_t timeout_to_ticks(std::uint32_t timeout_ms) noexcept;
        /** Sends at most one frame; called exclusively by the TX task. */
        bool service_tx() noexcept;
        /** Used by connection managers before restarting a physical client. */
        [[nodiscard]] bool tx_idle() noexcept;

    private:
        static constexpr std::size_t QUEUE_SIZE = 131072;
        static constexpr std::size_t LOG_QUEUE_SIZE = 4096;
        static constexpr std::size_t MAX_LOG_FRAME_SIZE = sizeof(msg_header_t) + sizeof(log_data_t) + 1024;
        static constexpr std::size_t TX_TASK_STACK_SIZE = 4096;
        static constexpr UBaseType_t TX_TASK_PRIORITY = 3;
        static constexpr std::uint32_t POLL_MS = 10;
        static constexpr char TAG[] = "sidp_tq";

        void close_locked() noexcept;
        static void drain(RingbufHandle_t ring) noexcept;
        esp_err_t enqueue(std::span<const std::uint8_t> message, bool log) noexcept;
        static void tx_task_trampoline(void *arg) noexcept;
        void tx_task_loop() noexcept;

        /**
         * @brief RAII guard over the statically allocated queue mutex.
         *
         * The mutex uses caller-provided storage (StaticSemaphore_t) so it never
         * allocates on the heap; on ESP-IDF a std::mutex would lazily malloc its
         * pthread control block on first lock. The handle is null before
         * create_queues() runs, in which case there is no shared state to protect
         * yet and the guard is a no-op.
         */
        class queue_guard final
        {
        public:
            explicit queue_guard(SemaphoreHandle_t handle) noexcept : mutex(handle)
            {
                if (mutex != nullptr) {
                    (void)xSemaphoreTake(mutex, portMAX_DELAY);
                }
            }

            ~queue_guard() noexcept
            {
                if (mutex != nullptr) {
                    (void)xSemaphoreGive(mutex);
                }
            }

            queue_guard(const queue_guard &) = delete;
            queue_guard &operator=(const queue_guard &) = delete;

        private:
            SemaphoreHandle_t mutex;
        };

        SemaphoreHandle_t queue_mutex = nullptr;
        StaticSemaphore_t queue_mutex_storage{};
        RingbufHandle_t ring_buffer = nullptr;
        RingbufHandle_t tx_ring = nullptr;
        RingbufHandle_t log_ring = nullptr;
        TaskHandle_t tx_task_handle = nullptr;
        std::uint64_t link_serial = 0;
        std::uint64_t accepted_link_serial = 0;
        std::size_t rx_borrowed = 0;
        std::size_t tx_pending = 0;
        bool tx_busy = false;
        std::atomic<bool> dead{true};
        std::atomic<bool> connected{false};
        std::atomic<std::uint32_t> epoch{0};
        std::atomic<std::uint32_t> tx_dropped{0};
    };
}
