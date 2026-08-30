#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"

#include "sidp_transport_queue.hpp"

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
     * message. A complete reassembled packet is validated and queued by the
     * packet_queue_transport base.
     *
     * The Espressif client owns connection establishment, TLS, keepalive, and
     * optional automatic reconnect. This class only maps binary messages to
     * the transport_intf message interface.
     *
     * init() performs all transport-owned allocation. One reader and one
     * writer may operate concurrently; callers must serialize writers.
     */
    class websocket_transport final : public packet_queue_transport
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
         * @return ESP_OK on success.
         * @return ESP_ERR_INVALID_STATE if already initialized.
         * @return ESP_ERR_INVALID_ARG if the configuration is invalid.
         * @return ESP_ERR_NO_MEM if initialization memory cannot be allocated.
         * @return ESP_FAIL if the WebSocket client cannot be started.
         */
        [[nodiscard]] esp_err_t init(const esp_websocket_client_config_t &config) noexcept;

        /** @copydoc transport_intf::write_message */
        [[nodiscard]] esp_err_t write_message(std::span<const std::uint8_t> message) noexcept override;

        /** @copydoc transport_intf::flush_write */
        [[nodiscard]] esp_err_t flush_write(std::uint32_t timeout_ms) noexcept override;

        /** @copydoc transport_intf::is_open */
        [[nodiscard]] bool is_open() const noexcept override;

    private:
        websocket_transport() noexcept = default;
        ~websocket_transport() override = default;

        /** @brief Handles events from esp_websocket_client. */
        static void websocket_event_handler(void *handler_arg, esp_event_base_t event_base, std::int32_t event_id, void *event_data) noexcept;

        /** @brief Handles one WebSocket data callback. */
        void handle_data(const esp_websocket_event_data_t &data) noexcept;

        /** @brief Clears the currently staged message. */
        void reset_staging() noexcept;

        /** @brief Discards pending output after a disconnect. */
        void discard_pending_tx() noexcept;

        esp_websocket_client_handle_t client = nullptr;
        std::uint8_t *staging_buffer = nullptr;
        std::uint8_t *tx_buffer = nullptr;
        std::size_t staging_received = 0;
        std::size_t tx_size = 0;
        bool staging_active = false;
        bool staging_discarded = false;
        bool initialized = false;

        static constexpr char TAG[] = "sidp_ws";
    };

}
