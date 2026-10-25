idf_component_register(
    SRCS "src/bridge.c" "src/bridge_codec.c"
    INCLUDE_DIRS "include"
    REQUIRES uart_service mqtt_service remote_log pkt_codec ack_manager common cjson
)
