cmake_minimum_required(VERSION 3.30)
project(serial_pkt_protocol C)

set(CMAKE_C_STANDARD 11)

include_directories(include)

add_executable(serial_pkt_protocol
        src/main.c
        src/ring_buffer.c
        src/pkt_protocol.c
        src/pkt_protocol_buf.c
        src/mqtt_utils.c
        include/mqtt_utils.h
        include/ctrl_protocol.h
)


# 添加测试子目录（仅在启用测试时编译）
option(BUILD_TESTING "Build tests" ON)
if (BUILD_TESTING)
    add_subdirectory(tests)  # 引用测试专用的 CMakeLists.txt
endif ()