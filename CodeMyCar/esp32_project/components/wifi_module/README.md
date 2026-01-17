### 如何使用
```C
#include "wifi_manager.h"
#include "app_events.h"

void app_main() {
    // 初始化应用事件系统
    app_events_init();

    // 初始化并启动WiFi模块
    wifi_init_and_start();

}
```
### 调用流程图描述

- 调用 wifi_init 初始化 WiFi 模块。
- 调用 wifi_start_connect 启动 WiFi 连接任务。
- 在 WiFi 连接任务中，注册事件处理函数并启动 WiFi。
- 根据 WiFi 事件（如连接成功、断开连接等），触发相应的处理逻辑。
- 如果需要停止 WiFi，调用 wifi_stop_connect。

### PlantUML
```puml
@startuml
start
:调用 wifi_init();
if (事件组是否已创建?) then (否)
    :创建事件组 s_wifi_event_group;
endif
:初始化 TCP/IP 堆栈;
:创建默认事件循环;
:创建默认 WiFi Station 接口;
:初始化 WiFi 驱动;
:设置 WiFi 模式为 Station 模式;

:调用 wifi_start_connect();
:createTask(wifi_connect_task);

partition "WiFi 连接任务" {
    :注册 WiFi 和 IP 事件处理函数;
    :配置 WiFi 参数;
    :启动 WiFi;
    while (WiFi 未连接) is (是)
        if (WiFi 断开连接) then (是)
            :尝试重新连接;
            if (重试次数超过限制?) then (是)
                :设置 EVENT_WIFI_DISCONNECTED;
            else
                :继续重试;
            endif
        endif
        if (获取到 IP 地址) then (是)
            :设置 EVENT_WIFI_CONNECTED;
        endif
    endwhile (否)
    :删除任务;
}

:调用 wifi_stop_connect();
:停止 WiFi;
stop
@enduml
```
