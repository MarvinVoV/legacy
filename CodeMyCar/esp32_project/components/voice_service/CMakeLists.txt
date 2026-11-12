set(voice_sources
    "src/voice_service.c"
    "src/voice_audio.c"
    "src/voice_codec.c"
    "src/voice_codec_i2s.c"
    "src/voice_codec_board.c"
    "src/voice_output.c"
    "src/voice_transport.c"
    "src/voice_protocol.c"
    "src/voice_state.c"
    "src/voice_wake_ack.c")
if(IDF_TARGET STREQUAL "esp32s3")
    list(APPEND voice_sources "src/voice_frontend_s3.c")
else()
    list(APPEND voice_sources "src/voice_wake.c")
endif()
idf_component_register(
    SRCS ${voice_sources}
    INCLUDE_DIRS "include"
    EMBED_FILES "assets/wake_ack.pcm"
    REQUIRES
        esp_driver_i2s
        esp_driver_i2c
        esp_driver_gpio
        esp_driver_uart
        esp_websocket_client
        esp_codec_dev
        cjson
        esp-sr
        wifi_service
        common
)

target_link_libraries(${COMPONENT_LIB} m)
