# 说明

## 1. 项目目录

```bash
esp_http/
├── CMakeLists.txt
├── sdkconfig
├── components/
│   ├── wifi/
│   │   ├── include/
│   │   │   └── wifi_manager.h
│   │   ├── src/
│   │   │   └── wifi_manager.c
│   │   └── CMakeLists.txt
│   ├── my_mqtt/
│   │   ├── include/
│   │   │   └── mqtt_manager.h
│   │   ├── src/
│   │   │   └── mqtt_manager.c
│   │   └── CMakeLists.txt
│   └── common/
│       ├── include/
│       │   └── app_events.h
│       ├── src/
│       │   └── app_events.c
│       └── CMakeLists.txt
└── main/
    ├── CMakeLists.txt
    ├── src/
    │   └── main.c
    └── build/
```

### 项目结构说明

#### wifi_manager 模块

职责：处理 WiFi 连接状态机、事件处理、自动重连

关键技术点：

- 使用事件组同步连接状态
- 使用 FreeRTOS 任务进行异步连接管理
- 通过回调函数通知连接状态变化

#### my_mqtt 模块

职责: 处理 MQTT 连接状态、事件处理、自动重连
关键技术点：

- 使用 FreeRTOS 事件组进行同步通信

#### app_events 模块

职责：定义全局事件位和共享数据结构
包含：

- 系统级事件位定义（WiFi 连接、HTTP 完成等）
- 共享数据结构（配置参数等）

## 2.关于调度任务

### 2.1 运行时查看任务调度情况

```bash
idf.py monitor
```

在监控界面输入 `tasks` 查看所有任务状态
Task Name | State | Priority | Stack
wifi | R | 5 | 2048
http | B | 6 | 4096

### 关于 MQTT

启动 MQTT 服务端：

```shell
/opt/homebrew/opt/mosquitto/sbin/mosquitto -c /opt/homebrew/etc/mosquitto/mosquitto.conf
```

测试环节，配置文件调整:

- allow_zero_length_clientid true
- allow_anonymous true
- persistent_client_expiration 14d
- listener 1883 0.0.0.0
- allow_anonymous true
- persistence true

### menuconfig

1. 开启 CONFIG_UART_ISR_IN_IRAM 配置项,解释:

   a. ESP_INTR_FLAG_IRAM：当设置此标志时，意味着中断服务程序（ISR）将尝试从 IRAM（指令 RAM）而不是 Flash 中执行。这可以减少由于 Flash 访问延迟导致的中断响应时间增加的问题。

   b. CONFIG_UART_ISR_IN_IRAM：这是一个配置选项，允许或禁止 UART ISR 在 IRAM 中运行。如果该选项没有启用，则即使设置了 ESP_INTR_FLAG_IRAM，ISR 也不会从 IRAM 运行。

2. 配置 MQTT Configuration
3. 配置 WiFi Configuration
4. Compiler options 开启优化
   a. Optimization Level (Debug(-0g)) --> Optimize for size (-0s with GCC, -0z with Clang)
5. Partition Table 开启优化
   a. Partition Table (Single factory app. no OTA) --> Single factory app(Large). no OTA

## 项目环境重置

1. 删除 build、CMakeCache.txt、skdconfig、CMakeFiles 文件夹内
2. 执行深度清理

```bash
idf.py fullclean
```

3. 设置芯片类型

```bash
idf.py set-target esp32c6
```

4. 配置 menuconfig

```bash
idf.py menuconfig
```

5. 编译

```bash
idf.py build
```

6. 烧录

```bash
idf.py flash
```

或者烧录后直接打开串口

```bash
idf.py -p /dev/cu/xxxx flash monitor
```

7. vscode 编辑器配置
   a. 首次通过 vscode 时，需要安装 esp-idf 插件，然后点击 esp-idf extension，选择 esp32c6，然后点击 install，安装完成后重启 vscode
   b. cmd+shift+p，输入 esp-idf，选择 esp-idf: Select OpenCDO Board Configuration，选择 esp32c6，然后点击 ok
   c. cmd+shift+p，输入 esp-idf，选择 esp-idf: Select Flash Method, 选择 UART
   d. cmd+shift+p，输入 esp-idf，选择 esp-idf: Add vscode Configuration Folder.  重要：会创建.vscode 目录以及 c_cpp_properties.json 文件，使得 vscode 可以识别代码库，智能提示、代码补全等。

## MQTT
开启mqtt服务
```shell
/opt/homebrew/opt/mosquitto/sbin/mosquitto -c /opt/homebrew/etc/mosquitto/mosquitto.conf
```
