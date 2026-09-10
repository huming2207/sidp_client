#pragma once
#include <cstdint>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
using esp_event_base_t = const char *;
using esp_event_handler_t = void (*)(void *, esp_event_base_t, std::int32_t, void *);
inline constexpr int WEBSOCKET_EVENT_ANY = -1, WEBSOCKET_EVENT_CONNECTED = 1,
    WEBSOCKET_EVENT_DISCONNECTED = 2, WEBSOCKET_EVENT_ERROR = 3, WEBSOCKET_EVENT_DATA = 4,
    WEBSOCKET_EVENT_CLOSED = 5, WEBSOCKET_EVENT_FINISH = 6;
inline constexpr int WS_TRANSPORT_OPCODES_BINARY = 2, WS_TRANSPORT_OPCODES_CONT = 0;
struct esp_websocket_client_config_t {
    int buffer_size = 0;
    bool disable_auto_reconnect = false;
    bool enable_close_reconnect = true;
};
struct esp_websocket_event_data_t {
    int payload_len = 0, payload_offset = 0, data_len = 0;
    char *data_ptr = nullptr;
    int op_code = 2;
    bool fin = true;
};
struct host_ws {
    bool connected = false;
    bool disable_auto_reconnect = false;
    bool enable_close_reconnect = true;
    int events = 0;
    esp_event_handler_t callback = nullptr;
    void *arg = nullptr;
};
using esp_websocket_client_handle_t = host_ws *;
inline host_ws host_websocket;
inline void host_ws_event(int event, void *data = nullptr) {
    if (event == WEBSOCKET_EVENT_CONNECTED) host_websocket.connected = true;
    if (event == WEBSOCKET_EVENT_DISCONNECTED) host_websocket.connected = false;
    if (host_websocket.callback && (host_websocket.events == WEBSOCKET_EVENT_ANY || host_websocket.events == event))
        host_websocket.callback(host_websocket.arg, "WS", event, data);
}
inline auto esp_websocket_client_init(const esp_websocket_client_config_t *config) {
    host_websocket.disable_auto_reconnect = config->disable_auto_reconnect;
    host_websocket.enable_close_reconnect = config->enable_close_reconnect;
    return &host_websocket;
}
inline esp_err_t esp_websocket_register_events(host_ws *client, int events, esp_event_handler_t cb, void *arg) {
    client->events = events; client->callback = cb; client->arg = arg; return ESP_OK;
}
inline esp_err_t esp_websocket_unregister_events(host_ws *client, int, esp_event_handler_t) { client->callback = nullptr; return ESP_OK; }
inline esp_err_t esp_websocket_client_start(host_ws *) { return ESP_OK; }
inline esp_err_t esp_websocket_client_stop(host_ws *ws) {
    if (!ws->connected) return ESP_FAIL;
    host_ws_event(WEBSOCKET_EVENT_DISCONNECTED); return ESP_OK;
}
inline esp_err_t esp_websocket_client_destroy(host_ws *) { return ESP_OK; }
inline bool esp_websocket_client_is_connected(host_ws *client) { return client->connected; }
inline int esp_websocket_client_send_bin(host_ws *, const char *, int size, TickType_t) { return size; }
