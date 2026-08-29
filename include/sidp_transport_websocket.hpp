#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/ringbuf.h"

#include "sidp_transport.hpp"

namespace sidp
{

    /**
     * @brief SIDP transport over the Espressif WebSocket client.
     *
     * One binary WebSocket message carries one SIDP message. If the peer
     * sends that message as protocol-level fragments, or if Espressif splits
     * one WebSocket frame across multiple data callbacks, the callback
     * handler reassembles both in a fixed staging buffer: fragments are
     * accumulated until the frame carrying the FIN bit completes, and
     * per-callback chunk offsets are tracked separately. Interleaved control
     * frames (PING/PONG/CLOSE) are ignored without disturbing the staged
     * message. A complete reassembled packet is validated and then queued as
     * one NOSPLIT ring-buffer item.
     *
     * The Espressif client owns connection establishment, TLS, keepalive, and
     * optional automatic reconnect. This class only maps binary messages to
     * the transport_intf message interface.
     *
     * init() performs all transport-owned allocation. One reader and one
     * writer may operate concurrently; callers must serialize writers.
     */
    class websocket_transport final : public transport_intf
    {
    public:
        /** @brief Returns the process-wide WebSocket transport instance. */
        [[nodiscard]] static websocket_transport &instance() noexcept
        {
            static websocket_transport transport;
            return transport;
        }

        websocket_transport(const websocket_transport &) = delete;
        websocket_transport &operator=(const websocket_transport &) = delete;

        /**
         * @brief Initializes and starts the Espressif WebSocket client.
         *
         * The receive buffer size is raised to MAX_FRAME_SIZE when necessary,
         * so one SIDP frame never requires protocol-level fragmentation.
         * Pointer-backed TLS configuration data must remain valid for the
         * lifetime required by esp_websocket_client.
         *
         * @param config Espressif WebSocket client configuration.
         * @return 0 on success.
         * @return -EALREADY if already initialized.
         * @return -EINVAL if the configuration is invalid.
         * @return -ENOMEM if initialization memory cannot be allocated.
         * @return -EIO if the WebSocket client cannot be started.
         */
        [[nodiscard]] int init(const esp_websocket_client_config_t &config) noexcept;

        /** @copydoc transport_intf::start_read */
        [[nodiscard]] int start_read(std::uint8_t **buf_out, std::size_t *len_out, std::uint32_t timeout_ms) noexcept override;

        /** @copydoc transport_intf::end_read */
        void end_read(std::uint8_t *buf_return) noexcept override;

        /** @copydoc transport_intf::write_message */
        [[nodiscard]] int write_message(std::span<const std::uint8_t> message) noexcept override;

        /** @copydoc transport_intf::flush_write */
        [[nodiscard]] int flush_write(std::uint32_t timeout_ms) noexcept override;

        /** @copydoc transport_intf::is_open */
        [[nodiscard]] bool is_open() const noexcept override;

    private:
        websocket_transport() noexcept = default;
        ~websocket_transport() override = default;

        /** @brief Capacity of the decoded-packet ring buffer in PSRAM. */
        inline static constexpr std::size_t RX_PACKET_RING_BUFFER_SIZE = 131072;

        /** @brief The receive ring buffer overflowed. */
        inline static constexpr EventBits_t EVENT_RX_OVERFLOW = BIT0;

        /** @brief Handles events from esp_websocket_client. */
        static void websocket_event_handler(void *handler_arg, esp_event_base_t event_base, std::int32_t event_id, void *event_data) noexcept;

        /** @brief Handles one WebSocket data callback. */
        void handle_data(const esp_websocket_event_data_t &data) noexcept;

        /** @brief Clears the currently staged message. */
        void reset_staging() noexcept;

        /** @brief Discards pending output after a disconnect. */
        void discard_pending_tx() noexcept;

        /** @brief Converts milliseconds to FreeRTOS ticks. */
        [[nodiscard]] static TickType_t timeout_to_ticks(std::uint32_t timeout_ms) noexcept;

        esp_websocket_client_handle_t client = nullptr;
        EventGroupHandle_t events = nullptr;
        RingbufHandle_t ring_buffer = nullptr;
        std::uint8_t *staging_buffer = nullptr;
        std::uint8_t *tx_buffer = nullptr;
        std::size_t staging_received = 0;
        std::size_t tx_size = 0;
        bool staging_active = false;
        bool staging_discarded = false;
        bool initialized = false;
    };

}
