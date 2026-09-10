#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
enum tinyusb_cdcacm_itf_t { TINYUSB_CDC_ACM_0 = 0, TINYUSB_CDC_ACM_MAX = 1 };
enum cdcacm_event_type_t { CDC_EVENT_RX, CDC_EVENT_LINE_STATE_CHANGED };
struct cdcacm_event_t {
    cdcacm_event_type_t type;
    struct { bool dtr = false, rts = false; } line_state_changed_data;
};
using tusb_cdcacm_callback_t = void (*)(int, cdcacm_event_t *);
struct tinyusb_config_cdcacm_t {
    tinyusb_cdcacm_itf_t cdc_port;
    tusb_cdcacm_callback_t callback_rx, callback_rx_wanted_char, callback_line_state_changed, callback_line_coding_changed;
};
inline tinyusb_config_cdcacm_t host_cdc_config{};
inline bool host_cdc_connected = false;
inline std::deque<std::uint8_t> host_cdc_input;
inline bool tud_cdc_n_connected(std::uint8_t) { return host_cdc_connected; }
inline bool tinyusb_cdcacm_initialized(tinyusb_cdcacm_itf_t) { return true; }
inline void tud_cdc_n_read_flush(std::uint8_t) { host_cdc_input.clear(); }
inline bool tud_cdc_n_write_clear(std::uint8_t) { return true; }
inline esp_err_t tinyusb_cdcacm_init(const tinyusb_config_cdcacm_t *config) { host_cdc_config = *config; return ESP_OK; }
inline esp_err_t tinyusb_cdcacm_unregister_callback(tinyusb_cdcacm_itf_t, cdcacm_event_type_t) { return ESP_OK; }
inline std::size_t tinyusb_cdcacm_write_queue(tinyusb_cdcacm_itf_t, const void *, std::size_t size) { return size; }
inline esp_err_t tinyusb_cdcacm_write_flush(tinyusb_cdcacm_itf_t, TickType_t) { return ESP_OK; }
inline esp_err_t tinyusb_cdcacm_read(tinyusb_cdcacm_itf_t, std::uint8_t *data, std::size_t size, std::size_t *read) {
    *read = std::min(size, host_cdc_input.size());
    for (std::size_t i = 0; i < *read; ++i) { data[i] = host_cdc_input.front(); host_cdc_input.pop_front(); }
    return ESP_OK;
}
inline void host_cdc_dtr(bool dtr) {
    host_cdc_connected = dtr;
    cdcacm_event_t event{CDC_EVENT_LINE_STATE_CHANGED, {dtr, false}};
    host_cdc_config.callback_line_state_changed(0, &event);
}
