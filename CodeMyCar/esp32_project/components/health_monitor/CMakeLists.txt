idf_component_register(
    SRCS "src/health_monitor.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_system esp_timer mqtt_service common
)
