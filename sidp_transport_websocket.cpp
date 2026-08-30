#include "sidp_transport_websocket.hpp"

#include <cerrno>
#include <cstring>

#include "esp_heap_caps.h"

namespace sidp
{

    int websocket_transport::init(const esp_websocket_client_config_t &config) noexcept
    {
        if (initialized) {
            return -EALREADY;
        }

        auto *input_buffer = static_cast<std::uint8_t *>(heap_caps_malloc(MAX_FRAME_SIZE, MALLOC_CAP_SPIRAM));
        if (input_buffer == nullptr) {
            return -ENOMEM;
        }

        auto *output_buffer = static_cast<std::uint8_t *>(heap_caps_malloc(MAX_FRAME_SIZE, MALLOC_CAP_SPIRAM));
        if (output_buffer == nullptr) {
            heap_caps_free(input_buffer);
            return -ENOMEM;
        }

        const int queue_result = create_packet_queue();
        if (queue_result != 0) {
            heap_caps_free(output_buffer);
            heap_caps_free(input_buffer);
            return queue_result;
        }

        esp_websocket_client_config_t client_config = config;
        if (client_config.buffer_size < static_cast<int>(MAX_FRAME_SIZE)) {
            client_config.buffer_size = static_cast<int>(MAX_FRAME_SIZE);
        }

        esp_websocket_client_handle_t websocket_client = esp_websocket_client_init(&client_config);
        if (websocket_client == nullptr) {
            destroy_packet_queue();
            heap_caps_free(output_buffer);
            heap_caps_free(input_buffer);
            return -EINVAL;
        }

        if (esp_websocket_register_events(websocket_client, WEBSOCKET_EVENT_DATA, websocket_event_handler, this) != ESP_OK) {
            esp_websocket_client_destroy(websocket_client);
            destroy_packet_queue();
            heap_caps_free(output_buffer);
            heap_caps_free(input_buffer);
            return -EIO;
        }

        client = websocket_client;
        staging_buffer = input_buffer;
        tx_buffer = output_buffer;
        initialized = true;

        if (esp_websocket_client_start(client) != ESP_OK) {
            esp_websocket_unregister_events(websocket_client, WEBSOCKET_EVENT_DATA, websocket_event_handler);
            esp_websocket_client_destroy(websocket_client);
            initialized = false;
            client = nullptr;
            staging_buffer = nullptr;
            tx_buffer = nullptr;
            destroy_packet_queue();
            heap_caps_free(output_buffer);
            heap_caps_free(input_buffer);
            return -EIO;
        }

        return 0;
    }

    int websocket_transport::write_message(std::span<const std::uint8_t> message) noexcept
    {
        if (!is_open()) {
            discard_pending_tx();
            return -ENOTCONN;
        }
        if (message.size() < sizeof(msg_header_t)) {
            return -EINVAL;
        }
        if (message.size() > MAX_FRAME_SIZE) {
            return -EMSGSIZE;
        }
        if (tx_size != 0) {
            return -EBUSY;
        }

        std::memcpy(tx_buffer, message.data(), message.size());
        tx_size = message.size();
        return 0;
    }

    int websocket_transport::flush_write(std::uint32_t timeout_ms) noexcept
    {
        if (tx_size == 0) {
            return 0;
        }
        if (!is_open()) {
            discard_pending_tx();
            return -ENOTCONN;
        }

        const int sent =
            esp_websocket_client_send_bin(client, reinterpret_cast<const char *>(tx_buffer), static_cast<int>(tx_size), timeout_to_ticks(timeout_ms));
        if (sent == static_cast<int>(tx_size)) {
            discard_pending_tx();
            return 0;
        }
        if (!is_open()) {
            discard_pending_tx();
            return -ENOTCONN;
        }
        if (sent < 0) {
            return -ETIMEDOUT;
        }

        discard_pending_tx();
        return -EIO;
    }

    bool websocket_transport::is_open() const noexcept
    {
        return initialized && esp_websocket_client_is_connected(client);
    }

    void
    websocket_transport::websocket_event_handler(void *handler_arg, esp_event_base_t event_base, std::int32_t event_id, void *event_data) noexcept
    {
        (void)event_base;
        if (handler_arg == nullptr || event_id != WEBSOCKET_EVENT_DATA || event_data == nullptr) {
            return;
        }

        static_cast<websocket_transport *>(handler_arg)->handle_data(*static_cast<esp_websocket_event_data_t *>(event_data));
    }

    void websocket_transport::handle_data(const esp_websocket_event_data_t &data) noexcept
    {
        if (!initialized) {
            return;
        }
        if (data.payload_len < 0 || data.payload_offset < 0 || data.data_len < 0 ||
            (data.data_len != 0 && data.data_ptr == nullptr)) {
            return;
        }

        /* RFC 6455: control frames may interleave with data fragments. They
         * are never part of a SIDP message and must not disturb reassembly. */
        const bool is_binary = data.op_code == WS_TRANSPORT_OPCODES_BINARY;
        const bool is_continuation = data.op_code == WS_TRANSPORT_OPCODES_CONT;
        if (!is_binary && !is_continuation) {
            return;
        }

        /* The library resets payload_offset per WebSocket frame and reports
         * the frame's opcode and FIN bit for every chunk event of that frame.
         * Therefore payload_offset == 0 marks the first event of a frame:
         * only there may a new message start. */
        const std::size_t frame_size = static_cast<std::size_t>(data.payload_len);
        const std::size_t frame_offset = static_cast<std::size_t>(data.payload_offset);
        const std::size_t chunk_size = static_cast<std::size_t>(data.data_len);

        if (frame_offset == 0 && is_binary) {
            /* A new binary frame starts a new message. A still-staged
             * previous message is a peer protocol violation and is dropped. */
            reset_staging();
            staging_active = true;
        } else if (!staging_active) {
            /* Orphaned continuation, or a mid-frame chunk whose frame start
             * was never observed. */
            return;
        }

        if (chunk_size != 0 && !staging_discarded) {
            if (staging_received + chunk_size > MAX_FRAME_SIZE) {
                /* Overlong message: stop copying but keep consuming events
                 * until the FIN bit, so the next message starts clean. */
                staging_discarded = true;
            } else {
                std::memcpy(staging_buffer + staging_received, data.data_ptr, chunk_size);
            }
        }
        staging_received += chunk_size;

        const bool frame_complete = frame_offset + chunk_size >= frame_size;
        if (!data.fin || !frame_complete) {
            return;
        }

        /* The frame carrying FIN is fully consumed: the message is done. */
        if (!staging_discarded) {
            deliver_packet(staging_buffer, staging_received);
        }
        reset_staging();
    }

    void websocket_transport::reset_staging() noexcept
    {
        staging_received = 0;
        staging_active = false;
        staging_discarded = false;
    }

    void websocket_transport::discard_pending_tx() noexcept
    {
        tx_size = 0;
    }

}
