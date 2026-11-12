#pragma once

#include <atomic>
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
     * The Espressif client owns connection establishment, TLS and keepalive.
     * Reconnect is explicit, including after clean CLOSE events.
     *
     * The device stays connected between SIDP connections. The server marks
     * each one with text messages, which do what DTR does on USB: "open"
     * starts a SIDP connection and "close" ends it. The owner answers with
     * finish(): "ready" once accepted, "busy" when it cannot serve one now,
     * "closed" once the target is cleaned up.
     *
     * A WebSocket frame's header and payload are written separately, so with
     * Nagle's algorithm every frame would wait for the peer's delayed ACK.
     * When the caller supplies ext_transport, Nagle is turned off on its
     * socket at each connection.
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

        /**
         * Starts a new physical connection after failure. Call from the connection
         * manager after target cleanup; returns INVALID_STATE until TX is idle.
         * Automatic reconnect is disabled so a send can never cross connections.
         * Once connected, the owner must call begin_session().
         */
        [[nodiscard]] esp_err_t reconnect() noexcept;

        /** True from start or reconnect() until the attempt connects or fails. */
        [[nodiscard]] bool is_connecting() const noexcept { return connecting.load(); }

        /** The WebSocket itself is connected, whether or not a SIDP connection is open. */
        [[nodiscard]] bool server_connected() const noexcept;

        /**
         * Sends "ready" after begin_session() accepted an "open", or ends the SIDP
         * connection and sends @p reply: "busy" to refuse an "open", "closed"
         * after cleanup. Call from the owner task.
         */
        esp_err_t finish(const char *reply) noexcept;

    private:
        [[nodiscard]] bool physical_link_open() const noexcept override;
        websocket_transport() noexcept = default;
        ~websocket_transport() override = default;

        /** Power data for a few seconds of WiFi trouble; USB keeps the default. */
        inline static constexpr std::size_t WS_LOG_QUEUE_SIZE = 32768;

        /** @brief Maximum blocking interval for one send_bin() call. */
        inline static constexpr std::uint32_t WS_SEND_TIMEOUT_MS = 5000;

        /** @brief Handles events from esp_websocket_client. */
        static void websocket_event_handler(void *handler_arg, esp_event_base_t event_base, std::int32_t event_id, void *event_data) noexcept;

        /** @brief Handles one WebSocket data callback. */
        void handle_data(const esp_websocket_event_data_t &data) noexcept;

        /** Handles "open" and "close" from the server. */
        void handle_text(const esp_websocket_event_data_t &data) noexcept;

        /** @brief Clears the currently staged message. */
        void reset_staging() noexcept;

        /** @copydoc packet_queue_transport::deliver_tx_frame */
        [[nodiscard]] bool deliver_tx_frame(std::span<const std::uint8_t> frame, bool log) noexcept override;

        esp_websocket_client_handle_t client = nullptr;
        std::uint8_t *staging_buffer = nullptr;
        std::size_t staging_received = 0;
        std::uint32_t staging_epoch = 0;
        bool staging_active = false;
        bool staging_discarded = false;
        bool initialized = false;
        std::atomic<bool> connecting{false};
        std::atomic<bool> opened{false}; // "open" received, "close" not yet
        esp_transport_handle_t ext_transport = nullptr;

        static constexpr char TAG[] = "sidp_ws";
    };

}
