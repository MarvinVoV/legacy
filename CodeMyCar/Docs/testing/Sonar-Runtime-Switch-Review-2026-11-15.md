# 超声波运行期开关复核、修复与实机验收

## 修复与实机验收 · 2026-11-15 晚

以下审查保留了修复前的现象。当前接口与字节协议见[配置协议](../protocol/cmd-mapping.md#超声波运行期开关)，固定服务端口与启动方法见[运行手册](../DevOps.md)。

- STM32 Release 通过 ST-LINK 完整写入校验，随后复位。S3 经 CP2102N 的 UART USB 烧录、写入哈希校验和复位成功。台架验收时 STM32 独立供电，S3 仅 USB 供电，TB6612 关闭。
- 实机使用 Mosquitto 1883、pyserver 8000。写请求使用新的配置通道与 STM32 会话/代数保护。会话 1601085400、epoch 2747616801 内，关闭回执 811650924（代数 3），超过两个状态周期后仍报告关闭且前后距离均为 null。开启回执 1983590880（代数 4）先报告等待采样，随后两侧均报告 400 cm（量程上限），ready=true。再次开启回执 3819690290（代数 5），保持 ready=true。现场最终为开启。
- 初次实机验证脚本误从设备 status 响应根读取距离字段，触发 KeyError，清理步骤已重新开启保护。按 `data.front_distance_cm` 和 `data.rear_distance_cm` 修正后完整重复，结果如上。
- 维护回归：共享协议测试覆盖重复/过期请求、旧代数、时钟回绕、停车条件、NoSonar；S3 桥接覆盖严格编码与独立状态帧；pyserver 使用 FakeMQTT 和实际入站处理器验证 HTTP 状态、匹配回执和失效距离。完整回归 `python3 tools/regress.py all` 通过 16 组，含 pyserver 372 项测试。定向测试入口为 `python3 tools/regress.py protocol control bridge pyserver`。
- 保留的专项实机用例：分别复位两板核对恢复策略、并发配置与 STOP、近障持续时重复开启和蜂鸣器实际静音。本次没有驱动车轮或测试行驶避障。

## 修复前的审查基线

审查基线为 main 的 `4e058e8`，功能提交为 `500f7fc` 和 `c8123aa`。HTTP 可以发布配置，Release 固件有实际处理分支。设备生效状态尚无回读，且配置 ACK 可干扰运动 ACK。**先修复下表的 P1 项，再烧录 S3 验收。** 本次仅做离线验证。

## 现有契约

| 层 | 当前实际行为 |
|---|---|
| HTTP | `POST /api/v1/devices/{device_id}/config/sonar`，请求体 `{"enabled":false}`，也可传 true。严格布尔，缺字段、多字段、数字和字符串均拒绝。设备心跳在线且 STM32 连接标志不为 false 时允许发布 |
| MQTT | pyserver 向 `cmd/{id}/exec` 发布 `{"cmd":"config","sonar":false}`，默认 QoS 0。正常响应 `success=true, data={"sonar":false,"confirmed":false}` 表示发布步骤完成。未附配置请求 ID |
| UART | 新 S3 把上述 JSON 编成类型 `0x0B`、payload `01 00`（关闭）或 `01 01`（开启），含头和 CRC 共 10 字节。STM32 用普通 ACK 回状态码 |
| STM32 | Release 上电默认开启。关闭会停止周期测距和距离上报，同时撤掉该方向的超声限速/制动。开关在 RAM 中，复位 STM32 恢复默认；仅重启 pyserver 或 ESP32 不会重置运行中的 STM32 |
| Agent | HTTP 调用者可以发布开关。车载 Agent 的 `car_get_state` 能取传感器数据，目前没有读取生效开关或设置开关的原生工具 |

## 按优先级修复

| 优先级 | 问题、触发条件和影响 | 建议与验收条件 |
|---|---|---|
| P1 | S3 用灯效事件计数低 8 位作 CONFIG 的 wire seq，置 ACK_REQ，却没有登记配置事务。所有普通 ACK 仅按 seq 清运动 pending。无同号时配置 ACK 被丢弃；同号时可清掉 GOAL 或 STOP 的槽、上报错误的运动 ACK，并停止该槽的重传。可靠 STOP 在 pyserver 还会等待零速，不能把此缺陷解释成已绕过零速检查。[S3 发送与 ACK 分派](../../esp32_project/components/bridge/src/bridge.c) | CONFIG 用独立类型的响应（或设计可验证的类型化事务），携带请求标识、会话和实际状态，彻底隔离运动 ACK。验收包括同号并发、迟到、序号回绕，以及配置回执到达时 STOP 仍能正常重传并等待自己的 ACK |
| P1 | 即使已经开启，重复请求 true 也会重新初始化前后通道，清除 fastBlocked、有效距离和故障，并重启 3 秒启动宽限。运动层将尚无有效样本的通道视作可解除刹车；如果原 GOAL/速度目标仍有效，有机会恢复输出。行驶中主动关闭同样能撤销超声制动。3 秒是无有效数据的宽限上限，正常新样本可能更早重新判挡。[开关处理](../../stm32_project/Application/Tasks/SonarTask/Src/sonar_task.c) · [解除制动条件](../../stm32_project/Application/Services/Ctrl/Src/motion_service.c) | 将重复设置做成幂等操作。建议仅允许在已停车且无活动运动事务时关闭；重新开启后进入等待新样本状态，再报告保护就绪。测试近障已制动时重复 true、关闭请求与活动 GOAL 并发，以及 OFF→ON 时旧样本不能放行 |
| P1 | CtrlTask 在 setter 中改写两个工作通道，SonarTask 同时更新这些结构。发布快照的临界区仅保护复制，没有保护工作通道的双写。关闭分支跳过 `Buzzer_Update`；若最后一次 GPIO 设置为响，关闭本身不会静音。停用期间还可能留下切换前的测距结果。[测距循环](../../stm32_project/Application/Tasks/SonarTask/Src/sonar_task.c) · [蜂鸣 GPIO](../../stm32_project/Application/Modules/Buzzer/Src/buzzer.c) | CtrlTask 只提交期望状态，由 SonarTask 单独执行转换、清理在途测距并发布快照。在关闭转换中明确静音，在转换结束后回执。验证响声持续期间关闭和采样交错 |
| P1 | CONFIG 和灯光共用长度 1 的覆盖队列。两个消息在消费前相继到达，后一个会覆盖前一个。配置 UART 入队返回值也被忽略。一次“重新开启保护”可以悄然丢失，而 HTTP 早已返回发布成功。[队列和发送](../../esp32_project/components/bridge/src/bridge.c) | 隔离灯光覆盖队列，配置使用独立、可确认的提交路径。队满、过期、断链要有结果；可采用 QoS 1 配合幂等键，禁止用 MQTT retain 在重连后重放旧的关闭指令 |
| P2 | HTTP 离线为 200 + success=false，STM32 连接为 null 时仍发布，MQTT 断开或发布超时则抛未处理异常并返回 500。无配置 GET、无能力标志、成功体中的 sonar 仅回显所求值。NoSonar 固件也会 ACK 接受开启，编译开关仍然禁用实际保护。STM32 接受超过 2 字节和任意非零值；协议参考包也没有这项 CONFIG 定义和专测。[HTTP 路由](../../pyserver/app/api/routes/commands.py) · [STM32 解码](../../stm32_project/Application/Tasks/CtrlTask/Src/ctrl_task.c) · [编译开关](../../stm32_project/Application/System/Common/Inc/ultrasonic_config.h) | 严格检查长度、子命令和值；协商能力，提供明确的 unsupported、offline、busy、timeout。参考包、两板与 pyserver 共享相同的正反例。未知固件回通用 ACK 不足以证明支持 |
| P2 | 设备停发距离后，服务端仍返回缓存的前后距离及 age；无效值 `0xFFFF` 也能被转为 `65535 cm` 并暴露给 Agent。调用者无法区分关闭、旧样本、无效测距与链路失联。[设备状态](../../pyserver/app/services/device_manager.py) · [距离解码](../../esp32_project/components/bridge/src/bridge_codec.c) | 返回设备报告的开关、来源会话、样本新鲜度及每侧有效性。失效/过期的当前距离用 null；400 cm 保留项目的超量程语义。STM32 复位或失联后使旧确认失效（S3 session 单独不足以识别仅 STM32 重启） |

## 给 HTTP 客户端和 Agent 的建议契约

保留当前 POST 兼容既有调用。可新增幂等的 `PUT /api/v1/devices/{id}/config/sonar` 设置期望状态，GET 同路径读取实际状态。只入队返回 202 和 request_id；收到匹配的设备回执后再报告 applied。响应模型显式区分 requested_enabled、effective_enabled、ready、supported、stale 和错误码。比如“已开启但在等首组有效测距”应能表达为 effective_enabled=true、ready=false。离线返回 503，运动中拒绝关闭返回 409，同步等待设备超时返回 504。上述字段和端点属于建议设计。

建议先让 Agent 通过现有状态工具读到有效状态。若需要授权其调整开关，再注册受权限控制的本地工具，说明关闭同时撤销避障保护；普通聊天保持只读。避免让模型从距离停更或发布成功推断“保护已开启”。

## 本轮验证证据

- 使用 FastAPI TestClient、FakeMQTT 与故障替身运行 11 个审查探针。验证了 true/false 的实际主题与载荷、四种 422、离线 HTTP 200、未知板间状态仍发布、GET 405、两种发布异常均返回 500，以及 120 秒前的 65535/18 cm 仍可读回。探针隔离于工作树，未连接真实 broker。
- 使用正式 ESP32 编码器和 ACK 管理器的 C 探针，确认 0/1 各编码成 10 字节。构造 priority 槽 wire_seq=1/client_seq=42，同号 ACK 清除该槽、返回 42，后到 ACK 无匹配。此项复现表级行为；与 bridge 的真实发送/分派路径结合判定碰撞风险。
- 使用正式 SafetyChannel 代码，先输入 6 cm 形成阻挡，再模拟 setter 的同值重新初始化，得到 blocked=false、scale=1.0、everValid=0。实车制动释放和蜂鸣器问题本轮均未做物理复现。
- 回归命令为 `python3 tools/regress.py bridge pyserver` 和 `bash stm32_project/tests/host/run.sh ultrasonic_safety motion_transaction`。既有套件覆盖通用安全/运动与协议；当前仓库未发现专门覆盖运行期开关的端到端用例。

## 设备待办

- [x] 修复配置 ACK 隔离、可靠回读、重复开启及安全状态切换，补齐上述故障和竞态回归。
- [x] **ESP32-S3 和 STM32 Release 均已完成烧录。** 后续刷机仍须按[运行手册](../DevOps.md)断开 S3 模块外部 5V、接 UART USB、核对板型与端口、烧录并记录 esp_build；若变更了 STM32 协议/任务，配套更新 Release 固件。
- [x] 执行器断电时已验收“查询初始开启 → 关闭并收到设备回执 → 确认状态 API 不再返回旧距离 → 开启并收到新距离”。
- [ ] 仅复位 ESP32 与仅复位 STM32 分开验证；分别检查保留状态和恢复默认。距离停更不能单独作为关闭证据。
- [ ] 独立悬空验收配置与 STOP 并发、关闭时静音、重复开启保持近障保护，记录请求 ID、回执、状态新鲜度和最终停车。
