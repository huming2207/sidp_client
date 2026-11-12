#include "sidp_transport_websocket.hpp"

#include "esp_log.h"
#include "esp_transport.h"

#include <cstring>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

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

        const esp_err_t queue_result = create_queues(WS_LOG_QUEUE_SIZE);
        if (queue_result != ESP_OK) {
            ESP_LOGE(TAG, "init: queue creation failed");
            heap_caps_free(input_buffer);
            return queue_result;
        }

        esp_websocket_client_config_t client_config = config;
        client_config.disable_auto_reconnect = true;
        client_config.enable_close_reconnect = false;
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

        if (esp_websocket_register_events(websocket_client, WEBSOCKET_EVENT_ANY, websocket_event_handler, this) != ESP_OK) {
            ESP_LOGE(TAG, "init: event handler registration failed");
            esp_websocket_client_destroy(websocket_client);
            destroy_queues();
            heap_caps_free(input_buffer);
            return ESP_FAIL;
        }

        client = websocket_client;
        ext_transport = config.ext_transport;
        staging_buffer = input_buffer;
        initialized = true;

        connecting.store(true);
        if (esp_websocket_client_start(client) != ESP_OK) {
            connecting.store(false);
            ESP_LOGE(TAG, "init: esp_websocket_client_start failed");
            esp_websocket_unregister_events(websocket_client, WEBSOCKET_EVENT_ANY, websocket_event_handler);
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
            esp_websocket_client_stop(websocket_client);
            esp_websocket_unregister_events(websocket_client, WEBSOCKET_EVENT_ANY, websocket_event_handler);
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

    bool websocket_transport::deliver_tx_frame(std::span<const std::uint8_t> frame, bool log) noexcept
    {
        (void)log;
        if (!is_open()) {
            return false;
        }
        // Log frames get the full timeout too: the client aborts the connection when a send times
        // out, so a short timeout would turn a WiFi delay into a lost session. Logs are still
        // dropped when their queue is full, before they are sent.
        const int sent = esp_websocket_client_send_bin(client, reinterpret_cast<const char *>(frame.data()),
                                                       static_cast<int>(frame.size()), timeout_to_ticks(WS_SEND_TIMEOUT_MS));
        if (sent == static_cast<int>(frame.size())) {
            return true;
        }
        ESP_LOGE(TAG, "deliver_tx_frame: send failed (%d of %u bytes, open=%d)", sent,
                 static_cast<unsigned>(frame.size()), is_open());
        return false;
    }

    esp_err_t websocket_transport::reconnect() noexcept
    {
        close_session();
        if (!initialized || !tx_idle()) return ESP_ERR_INVALID_STATE;
        const esp_err_t stopped = esp_websocket_client_stop(client);
        // This API reports ESP_FAIL when the task has already stopped after
        // a disconnect. Starting again is valid in that case.
        if (stopped != ESP_OK && stopped != ESP_FAIL) return stopped;
        connecting.store(true);
        const esp_err_t started = esp_websocket_client_start(client);
        if (started != ESP_OK) connecting.store(false);
        return started;
    }

    bool websocket_transport::server_connected() const noexcept
    {
        return initialized && esp_websocket_client_is_connected(client);
    }

    bool websocket_transport::physical_link_open() const noexcept
    {
        return opened.load() && server_connected();
    }

    esp_err_t websocket_transport::finish(const char *reply) noexcept
    {
        if (strcmp(reply, "ready") != 0) {
            opened.store(false);
            link_changed(false);
        }
        if (!server_connected()) return ESP_ERR_INVALID_STATE;
        const int len = static_cast<int>(strlen(reply));
        return esp_websocket_client_send_text(client, reply, len, timeout_to_ticks(WS_SEND_TIMEOUT_MS)) == len ? ESP_OK : ESP_FAIL;
    }

    void websocket_transport::handle_text(const esp_websocket_event_data_t &data) noexcept
    {
        // Session messages are a few bytes and always arrive in one piece.
        if (!data.fin || data.payload_offset != 0 || data.data_len != data.payload_len) return;
        const std::size_t len = static_cast<std::size_t>(data.data_len);
        if (len == 4 && memcmp(data.data_ptr, "open", 4) == 0) {
            if (!opened.exchange(true)) {
                link_changed(true); // A new SIDP connection, as DTR asserted on USB
            }
        } else if (len == 5 && memcmp(data.data_ptr, "close", 5) == 0) {
            opened.store(false);
            link_changed(false);
        }
    }

    void
    websocket_transport::websocket_event_handler(void *handler_arg, esp_event_base_t event_base, std::int32_t event_id, void *event_data) noexcept
    {
        (void)event_base;
        if (handler_arg == nullptr) {
            return;
        }
        switch (event_id) {
        case WEBSOCKET_EVENT_CONNECTED: {
            auto *self = static_cast<websocket_transport *>(handler_arg);
            self->connecting.store(false);
            self->opened.store(false); // Wait for "open"
            self->link_changed(false);
            self->reset_staging();
            if (self->ext_transport != nullptr) {
                const int fd = esp_transport_get_socket(self->ext_transport);
                const int one = 1;
                if (fd < 0 || setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)) != 0) {
                    ESP_LOGW(TAG, "websocket_event_handler: cannot turn Nagle off");
                }
            }
            ESP_LOGI(TAG, "websocket_event_handler: websocket connected");
            return;
        }
        case WEBSOCKET_EVENT_CLOSED:
        case WEBSOCKET_EVENT_FINISH:
        case WEBSOCKET_EVENT_DISCONNECTED:
            ESP_LOGE(TAG, "websocket_event_handler: websocket disconnected");
            static_cast<websocket_transport *>(handler_arg)->connecting.store(false);
            static_cast<websocket_transport *>(handler_arg)->opened.store(false);
            static_cast<websocket_transport *>(handler_arg)->link_changed(false);
            static_cast<websocket_transport *>(handler_arg)->reset_staging();
            return;
        case WEBSOCKET_EVENT_ERROR:
            static_cast<websocket_transport *>(handler_arg)->connecting.store(false);
            static_cast<websocket_transport *>(handler_arg)->opened.store(false);
            static_cast<websocket_transport *>(handler_arg)->link_changed(false);
            static_cast<websocket_transport *>(handler_arg)->reset_staging();
            ESP_LOGE(TAG, "websocket_event_handler: websocket error event");
            return;
        case WEBSOCKET_EVENT_DATA:
            break;
        default:
            return;
        }

        if (event_data != nullptr) {
            static_cast<websocket_transport *>(handler_arg)->handle_data(*static_cast<esp_websocket_event_data_t *>(event_data));
        }
    }

    void websocket_transport::handle_data(const esp_websocket_event_data_t &data) noexcept
    {
        if (data.op_code == WS_TRANSPORT_OPCODES_TEXT) {
            handle_text(data);
            return;
        }

        const auto current_epoch = receive_epoch();
        if (staging_epoch != current_epoch || !is_open()) {
            reset_staging();
            staging_epoch = current_epoch;
        }
        if (!is_open()) return;
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

        if (frame_offset > frame_size || chunk_size > frame_size - frame_offset) {
            reset_staging();
            return;
        }
        if (chunk_size != 0 && !staging_discarded) {
            if (chunk_size > MAX_FRAME_SIZE - staging_received) {
                /* Overlong message: stop copying but keep consuming events
                 * until the FIN bit, so the next message starts clean. */
                ESP_LOGE(TAG, "handle_data: ws message exceeds frame size, dropped");
                staging_discarded = true;
            } else {
                std::memcpy(staging_buffer + staging_received, data.data_ptr, chunk_size);
            }
        }
        if (!staging_discarded) staging_received += chunk_size;

        const bool frame_complete = frame_offset + chunk_size >= frame_size;
        if (!data.fin || !frame_complete) {
            return;
        }

        /* The frame carrying FIN is fully consumed: the message is done. */
        if (!staging_discarded) {
            deliver_packet(staging_buffer, staging_received, current_epoch);
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
