set(voice_sources "src/voice_service.c")
if(IDF_TARGET STREQUAL "esp32s3")
    list(APPEND voice_sources "src/voice_frontend_s3.c")
else()
    list(APPEND voice_sources "src/voice_wake.c")
endif()
idf_component_register(
    SRCS ${voice_sources}
    INCLUDE_DIRS "include"
    REQUIRES
        esp_driver_i2s
        esp_driver_gpio
        esp_driver_uart
        esp_websocket_client
        cjson
        esp-sr
        wifi_service
        common
)

target_link_libraries(${COMPONENT_LIB} m)
