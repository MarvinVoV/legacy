idf_component_register(
    SRCS "src/voice_service.c"
    INCLUDE_DIRS "include"
    REQUIRES
        esp_driver_i2s
        esp_driver_uart
        esp_websocket_client
        cjson
        wifi_service
        common
)

target_link_libraries(${COMPONENT_LIB} m)
