idf_component_register(
    SRCS "src/mqtt_service.c" "src/mqtt_topic_match.c"
    INCLUDE_DIRS "include"
    REQUIRES mqtt esp_event common
)
