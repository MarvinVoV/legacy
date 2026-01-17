#pragma once

#define DEVICE_ID "DEV_1"
// ============== 订阅主题  ==============
// 下行通信主题
#define TOPIC_SERVER_CTRL ("cmd/esp32/+/exec")  // 服务器控制指令

// ============== 发布主题  ==============
// 上行通信主题
#define TOPIC_ESP32_SENSOR ("data/esp32/" DEVICE_ID "/sensor")  // 设备数据上报
#define TOPIC_ESP32_HEARTBEAT ("data/esp32/" DEVICE_ID "/heartbeat") // 心跳包
#define TOPIC_ESP32_LOG ("data/esp32/" DEVICE_ID "/log") // 日志

#define TOPIC_STM32_SENSOR ("data/stm32/" DEVICE_ID "/sensor")  // 设备数据上报
#define TOPIC_STM32_HEARTBEAT ("data/stm32/" DEVICE_ID "/heartbeat") // 心跳包
#define TOPIC_STM32_LOG ("data/stm32/" DEVICE_ID "/log") //  日志
