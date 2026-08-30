#include "sidp_transport_websocket.hpp"

#include "esp_log.h"

#include <cstring>

#include "esp_heap_caps.h"

namespace sidp
{

    esp_err_t websocket_transport::init(const esp_websocket_client_config_t &config) noexcept
    {
        if (initialized) {
            ESP_LOGE(TAG, "init: websocket transport already initialized");
            return ESP_ERR_INVALID_STATE;
        }

        auto *input_buffer = static_cast<std::uint8_t *>(heap_caps_malloc(MAX_FRAME_SIZE, MALLOC_CAP_SPIRAM));
        if (input_buffer == nullptr) {
            ESP_LOGE(TAG, "init: input buffer allocation failed");
            return ESP_ERR_NO_MEM;
        }

        auto *output_buffer = static_cast<std::uint8_t *>(heap_caps_malloc(MAX_FRAME_SIZE, MALLOC_CAP_SPIRAM));
        if (output_buffer == nullptr) {
            ESP_LOGE(TAG, "init: output buffer allocation failed");
            heap_caps_free(input_buffer);
            return ESP_ERR_NO_MEM;
        }

        const int queue_result = create_packet_queue();
        if (queue_result != ESP_OK) {
            ESP_LOGE(TAG, "init: rx queue creation failed");
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
            ESP_LOGE(TAG, "init: esp_websocket_client_init failed");
            destroy_packet_queue();
            heap_caps_free(output_buffer);
            heap_caps_free(input_buffer);
            return ESP_ERR_INVALID_ARG;
        }

        if (esp_websocket_register_events(websocket_client, WEBSOCKET_EVENT_DATA, websocket_event_handler, this) != ESP_OK) {
            ESP_LOGE(TAG, "init: event handler registration failed");
            esp_websocket_client_destroy(websocket_client);
            destroy_packet_queue();
            heap_caps_free(output_buffer);
            heap_caps_free(input_buffer);
            return ESP_FAIL;
        }

        client = websocket_client;
        staging_buffer = input_buffer;
        tx_buffer = output_buffer;
        initialized = true;

        if (esp_websocket_client_start(client) != ESP_OK) {
            ESP_LOGE(TAG, "init: esp_websocket_client_start failed");
            esp_websocket_unregister_events(websocket_client, WEBSOCKET_EVENT_DATA, websocket_event_handler);
            esp_websocket_client_destroy(websocket_client);
            initialized = false;
            client = nullptr;
            staging_buffer = nullptr;
            tx_buffer = nullptr;
            destroy_packet_queue();
            heap_caps_free(output_buffer);
            heap_caps_free(input_buffer);
            return ESP_FAIL;
        }

        ESP_LOGI(TAG, "init: websocket transport started");
        return ESP_OK;
    }

    esp_err_t websocket_transport::write_message(std::span<const std::uint8_t> message) noexcept
    {
        if (!is_open()) {
            discard_pending_tx();
            return ESP_ERR_INVALID_STATE;
        }
        if (message.size() < sizeof(msg_header_t)) {
            return ESP_ERR_INVALID_ARG;
        }
        if (message.size() > MAX_FRAME_SIZE) {
            return ESP_ERR_INVALID_SIZE;
        }
        if (tx_size != 0) {
            return ESP_ERR_INVALID_STATE;
        }

        std::memcpy(tx_buffer, message.data(), message.size());
        tx_size = message.size();
        return ESP_OK;
    }

    esp_err_t websocket_transport::flush_write(std::uint32_t timeout_ms) noexcept
    {
        if (tx_size == 0) {
            return ESP_OK;
        }
        if (!is_open()) {
            discard_pending_tx();
            return ESP_ERR_INVALID_STATE;
        }

        const int sent =
            esp_websocket_client_send_bin(client, reinterpret_cast<const char *>(tx_buffer), static_cast<int>(tx_size), timeout_to_ticks(timeout_ms));
        if (sent == static_cast<int>(tx_size)) {
            discard_pending_tx();
            return ESP_OK;
        }
        if (!is_open()) {
            discard_pending_tx();
            return ESP_ERR_INVALID_STATE;
        }
        if (sent < 0) {
            ESP_LOGE(TAG, "flush_write: ws send failed: %d", sent);
            return ESP_ERR_TIMEOUT;
        }

        discard_pending_tx();
        return ESP_FAIL;
    }

    bool websocket_transport::is_open() const noexcept
    {
        return initialized && esp_websocket_client_is_connected(client);
    }

    void
    websocket_transport::websocket_event_handler(void *handler_arg, esp_event_base_t event_base, std::int32_t event_id, void *event_data) noexcept
    {
        (void)event_base;
        if (handler_arg == nullptr || event_data == nullptr) {
            return;
        }
        switch (event_id) {
        case WEBSOCKET_EVENT_CONNECTED:
            ESP_LOGI(TAG, "websocket_event_handler: websocket connected");
            return;
        case WEBSOCKET_EVENT_DISCONNECTED:
            ESP_LOGE(TAG, "websocket_event_handler: websocket disconnected");
            return;
        case WEBSOCKET_EVENT_ERROR:
            ESP_LOGE(TAG, "websocket_event_handler: websocket error event");
            return;
        case WEBSOCKET_EVENT_DATA:
            break;
        default:
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
            ESP_LOGD(TAG, "handle_data: orphaned continuation frame ignored");
            return;
        }

        if (chunk_size != 0 && !staging_discarded) {
            if (staging_received + chunk_size > MAX_FRAME_SIZE) {
                /* Overlong message: stop copying but keep consuming events
                 * until the FIN bit, so the next message starts clean. */
                ESP_LOGE(TAG, "handle_data: ws message exceeds frame size, dropped");
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
        if (tx_size != 0) {
            ESP_LOGE(TAG, "discard_pending_tx: discarding %u pending tx bytes", static_cast<unsigned>(tx_size));
        }
        tx_size = 0;
    }

}
