cmake_minimum_required(VERSION 3.20)

# 定义测试可执行文件名
set(TEST_TARGET serial_pkt_protocol_tests)

# 添加测试可执行文件
add_executable(${TEST_TARGET}
        pkt_protocol_test.c          # 测试代码
        ../src/pkt_protocol.c
        ../src/pkt_protocol_buf.c
        ../vendor/unity/unity.c
        ../src/mqtt_utils.c
        ../include/mqtt_utils.h
        ../include/ctrl_protocol.h # Unity 框架源码
)

# 设置头文件包含路径
target_include_directories(${TEST_TARGET} PRIVATE
        ${CMAKE_SOURCE_DIR}/src     # 业务代码头文件
        ${CMAKE_SOURCE_DIR}/vendor/unity  # Unity 头文件
)


# 注册测试到 CTest（CLion 支持）
enable_testing()
add_test(NAME ${TEST_TARGET} COMMAND ${TEST_TARGET})