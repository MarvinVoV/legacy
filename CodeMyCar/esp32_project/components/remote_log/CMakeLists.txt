idf_component_register(
    SRCS "src/remote_log.c"
    INCLUDE_DIRS "include"
    REQUIRES mqtt_service common esp_timer
)
