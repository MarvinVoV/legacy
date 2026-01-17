idf_component_register(
    SRCS "src/uart_manager.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_uart esp_driver_gpio esp_ringbuf common mqtt_module
)