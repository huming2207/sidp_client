#pragma once
using esp_transport_handle_t = struct host_transport *;
inline int esp_transport_get_socket(esp_transport_handle_t) { return -1; }
