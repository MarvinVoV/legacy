# STOP 下行与会话确认链路复查

> 后续已实施修复并烧录：立即心跳、STOP会话重试和QoS1、旧会话隔离回归均已完成；最新验收与剩余项见 `Session-And-Motion-Fix-Validation-2026-11-08.md`。下文保留定位时的原始记录。

日期：2026-11-08。分支：`codex/stm32-control-safety-rereview`。

背景：ESP32 从 STM32 VCC 改为 USB 供电，并连接电脑。用户要求复查之前的 STOP 未确认现象。本次没有发送任何运动指令，也没有修改或烧录产品代码。

## 结论

1. 当前 MQTT→ESP32→STM32→ACK→停车状态闭环可以工作，稳定上线后连续10次STOP全部确认成功，耗时95.4–248.7ms。
2. 确认存在服务端会话同步缺陷：已知设备的ESP32重启后，在新心跳到达前，新会话ACK/运动状态会被当成旧消息丢弃。此时STOP可能已执行，但接口报“no STM32 ACK arrived”。
3. 不能把“接口等待不到ACK”直接等同于“STM32没有收到下行”。之前的初步判断需要按这一点修正。
4. 本次没有证明供电或UART接线存在故障；也不能仅凭10次成功排除所有网络延迟、压力和硬件间歇故障。此前所有超时是否均由该会话问题造成，尚无逐次原始ACK证据，不作全量归因。

## 实机证据

- USB供电后，STOP seq=3返回accepted=true、confirmed=true；独立MQTT订阅同时看到status=0的ACK和同seq、mode=0、零速运动状态。
- 后续测试曾连续10次返回stop_unconfirmed；随后独立订阅看到STM32状态seq=15，说明至少对应序号的STOP到达并被执行，不能判定所有失败均发生在物理下行。
- 观察到ESP32会话由4014291693变为86730793。USB串口打开可能伴随设备重启，因此不能把串口测试阶段直接当作稳定会话。
- 使用不操作DTR/RTS的串口读取方式：STOP请求后约90ms看到ESP32日志 `CMD downlink len=65`，约220ms收到完整成功响应（seq=16）。
- 稳定会话下再测试seq=17–26，10/10成功，耗时：218.2、200.1、132.6、127.4、104.4、125.7、95.4、126.5、248.7、108.7ms。
- 所有测试均保持车辆静止，没有前进、倒车、舵机或GOAL运动验收。

## 确认的服务端缺陷

相关代码：

- `pyserver/app/mqtt/handlers.py`：新session只在heartbeat中正常更新；ACK/motion若已有非空session且与消息不同，则直接忽略。
- `pyserver/app/services/motion_service.py`：STOP pending使用发出时缓存的session；on_ack/on_motion再次要求session匹配。
- `esp32_project/components/health_monitor/src/health_monitor.c`：heartbeat周期30s。

因此出现以下序列：

1. 服务端缓存旧会话A。
2. ESP32重启进入新会话B，UART和MQTT已经可用。
3. 用户发STOP，pending归属A。
4. STM32执行STOP，ESP32转发带B的ACK与零速状态。
5. 服务端因B≠A丢弃；接口等待1s后返回stop_unconfirmed。
6. 新heartbeat到达后切换到B，之后STOP恢复正常。

电脑端已用实际create_handlers/AgentMotionService复现：注入新会话status=0的ACK与零速状态，在新heartbeat前仍得到stop_unconfirmed；发送新heartbeat后，相同处理链返回accepted=true、confirmed=true。

临时复现脚本：`/private/tmp/cmcar_session_blindspot_20261108.py`。临时文件不作为永久测试依赖，以上输入输出和调用路径为后续补正式回归的依据。

## 严重性与修复建议

这是安全命令确认缺陷，需按高优先级处理。它并不证明硬件没有制动，但会破坏上层可靠STOP的判断，使控制者无法区分“未送达”与“已执行但回执被丢弃”。

建议分两端处理：

1. ESP32在MQTT连接/重连成功时立即发布会话握手/heartbeat，不等待周期30s；发布与重试需要可观察的成功结果。
2. 服务端显式维护会话同步状态，避免把未知新会话的回执一律解释成“没有ACK”；跨会话请求应给出明确的会话变化/重新同步结果。
3. STOP在新会话下的确认策略需单独设计，确保同seq、status=0、当前会话及新鲜mode=0/零速状态闭环。
4. 不能简单删除session匹配、把任意不同session ACK都当成功；否则会重新引入重启前旧ACK/状态误确认新命令的问题。
5. 正式回归覆盖：已有旧会话、ESP32重启、首心跳前STOP、重连未重启、迟到旧ACK、seq重用、服务端重启、握手丢包。

本次任务完成的是复查与定位，没有将上述会话握手修复实施到产品代码。先前STM32严重控制修复的其他未完成项仍见 `STM32-Critical-Control-Fix-Validation-2026-11-08.md`。
