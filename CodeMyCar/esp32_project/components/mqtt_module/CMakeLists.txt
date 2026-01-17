idf_component_register(
    SRCS
        "src/mqtt_manager.c"
        "src/mqtt_logger.c"
        "src/mqtt_utils.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_netif mqtt common
)