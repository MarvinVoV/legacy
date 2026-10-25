cmake_minimum_required(VERSION 3.20)

set(TEST_TARGET serial_pkt_protocol_tests)

add_executable(${TEST_TARGET}
        pkt_protocol_test.c
        ../src/pkt_protocol.c
        ../src/pkt_protocol_buf.c
        ../src/mqtt_utils.c
        ../vendor/unity/unity.c
)

target_include_directories(${TEST_TARGET} PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/vendor/unity
)

enable_testing()
add_test(NAME ${TEST_TARGET} COMMAND ${TEST_TARGET})

# bridge_codec 跨语言测试：pyserver JSON → C 转 13 字节 ControlCmd / 二进制 → JSON
set(BRIDGE_TEST_TARGET bridge_codec_tests)
add_executable(${BRIDGE_TEST_TARGET}
        test_bridge_codec.c
        ../src/bridge_codec.c
        ../src/pkt_protocol.c
        ../vendor/cjson/cJSON.c
)
target_include_directories(${BRIDGE_TEST_TARGET} PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/vendor/cjson
)
add_test(NAME ${BRIDGE_TEST_TARGET} COMMAND ${BRIDGE_TEST_TARGET})

# ack_manager 重传表逻辑单测（纯 C，无 FreeRTOS）
set(ACK_TEST_TARGET ack_manager_tests)
add_executable(${ACK_TEST_TARGET}
        test_ack_manager.c
        ../src/ack_manager.c
)
target_include_directories(${ACK_TEST_TARGET} PRIVATE
        ${CMAKE_SOURCE_DIR}/include
)
add_test(NAME ${ACK_TEST_TARGET} COMMAND ${ACK_TEST_TARGET})
