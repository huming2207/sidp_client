idf_component_register(
    SRCS "sidp_transport.cpp"
         "sidp_transport_queue.cpp"
         "sidp_transport_cdc.cpp"
         "sidp_transport_websocket.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_ringbuf esp_tinyusb tinyusb esp_timer esp_websocket_client
)
