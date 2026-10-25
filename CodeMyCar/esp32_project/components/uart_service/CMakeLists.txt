idf_component_register(
    SRCS "src/uart_service.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_uart esp_driver_gpio esp_ringbuf pkt_codec common
)
