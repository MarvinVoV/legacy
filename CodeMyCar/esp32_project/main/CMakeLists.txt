idf_component_register(
    SRCS "src/main.c"
    INCLUDE_DIRS ""
    REQUIRES
        common
        pkt_codec
        wifi_service
        mqtt_service
        uart_service
        remote_log
        bridge
        health_monitor
)
