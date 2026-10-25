idf_component_register(
    SRCS
        "src/cmcar_crc16.c"
        "src/cmcar_events.c"
    INCLUDE_DIRS "include"
    REQUIRES freertos log esp_event nvs_flash
)
