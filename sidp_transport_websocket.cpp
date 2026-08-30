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

        const esp_err_t queue_result = create_queues();
        if (queue_result != ESP_OK) {
            ESP_LOGE(TAG, "init: queue creation failed");
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
            destroy_queues();
            heap_caps_free(input_buffer);
            return ESP_ERR_INVALID_ARG;
        }

        if (esp_websocket_register_events(websocket_client, WEBSOCKET_EVENT_DATA, websocket_event_handler, this) != ESP_OK) {
            ESP_LOGE(TAG, "init: event handler registration failed");
            esp_websocket_client_destroy(websocket_client);
            destroy_queues();
            heap_caps_free(input_buffer);
            return ESP_FAIL;
        }

        client = websocket_client;
        staging_buffer = input_buffer;
        initialized = true;

        if (esp_websocket_client_start(client) != ESP_OK) {
            ESP_LOGE(TAG, "init: esp_websocket_client_start failed");
            esp_websocket_unregister_events(websocket_client, WEBSOCKET_EVENT_DATA, websocket_event_handler);
            esp_websocket_client_destroy(websocket_client);
            initialized = false;
            client = nullptr;
            staging_buffer = nullptr;
            destroy_queues();
            heap_caps_free(input_buffer);
            return ESP_FAIL;
        }

        const esp_err_t task_result = spawn_tx_task("sidp_tx_ws");
        if (task_result != ESP_OK) {
            ESP_LOGE(TAG, "init: tx task spawn failed");
            esp_websocket_unregister_events(websocket_client, WEBSOCKET_EVENT_DATA, websocket_event_handler);
            esp_websocket_client_destroy(websocket_client);
            initialized = false;
            client = nullptr;
            staging_buffer = nullptr;
            destroy_queues();
            heap_caps_free(input_buffer);
            return task_result;
        }

        ESP_LOGI(TAG, "init: websocket transport started");
        return ESP_OK;
    }

    bool websocket_transport::deliver_tx_frame(std::span<const std::uint8_t> frame) noexcept
    {
        if (!is_open()) {
            return false;
        }
        const int sent = esp_websocket_client_send_bin(client, reinterpret_cast<const char *>(frame.data()),
                                                       static_cast<int>(frame.size()), timeout_to_ticks(WS_SEND_TIMEOUT_MS));
        if (sent == static_cast<int>(frame.size())) {
            return true;
        }
        ESP_LOGE(TAG, "deliver_tx_frame: send failed (%d of %u bytes, open=%d)", sent,
                 static_cast<unsigned>(frame.size()), is_open());
        return false;
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
            static_cast<websocket_transport *>(handler_arg)->drain_tx();
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

}
