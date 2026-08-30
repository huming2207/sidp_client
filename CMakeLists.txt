idf_component_register(
    SRCS "sidp_transport.cpp"
         "sidp_transport_queue.cpp"
         "sidp_session.cpp"
         "sidp_transport_cdc.cpp"
         "sidp_transport_websocket.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_ringbuf esp_tinyusb tinyusb esp_timer esp_websocket_client esp_common log
    PRIV_COMPILE_OPTIONS "-DLOG_LOCAL_LEVEL=ESP_LOG_DEBUG"
)
