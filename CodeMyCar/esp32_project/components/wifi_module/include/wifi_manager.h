#pragma once


// WiFi初始化函数
void wifi_init(void);

// 开始WiFi连接
void wifi_start_connect(void);

// 停止WiFi连接
void wifi_stop_connect(void);

// 初始化并启动WiFi (合并方法)
void wifi_init_and_start(void);