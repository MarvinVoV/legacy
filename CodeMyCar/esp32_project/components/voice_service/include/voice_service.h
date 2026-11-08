#pragma once

#include "esp_err.h"

/** 初始化 I2S 麦克风、串口触发任务；WiFi 连接后可用 s 触发上传。 */
esp_err_t voice_service_start(void);
