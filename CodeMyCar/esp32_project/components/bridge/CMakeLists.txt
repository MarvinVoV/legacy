idf_component_register(
    SRCS "src/bridge.c" "src/bridge_codec.c"
    INCLUDE_DIRS "include" "../../../serial_pkt_protocol/shared"
    PRIV_REQUIRES voice_service
    REQUIRES uart_service mqtt_service remote_log pkt_codec ack_manager common cjson
)
