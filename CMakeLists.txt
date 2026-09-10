idf_component_register(
    SRCS "sidp_transport.cpp"
         "sidp_transport_queue.cpp"
         "sidp_session.cpp"
         "sidp_transport_cdc.cpp"
         "sidp_transport_websocket.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_ringbuf esp_tinyusb tinyusb esp_timer esp_websocket_client esp_common log
)

# The wire definitions use std::span and std::endian, so the component requires
# C++20 from every consumer. The parent project already builds with C++23.
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_20)

target_compile_options(${COMPONENT_LIB} PRIVATE "-DLOG_LOCAL_LEVEL=ESP_LOG_DEBUG")
