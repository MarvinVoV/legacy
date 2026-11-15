# S3与STM32悬空硬件回归（2026-11-11）

## 结论与条件

用户确认车架支稳、两驱动轮悬空、转向有余量；两板各自连接电脑，电机驱动板已上电。稳定测试窗口内，UART双向通信、ACK、运动遥测、低速正反转/转向、租约、GOAL、可靠STOP及S3重启会话恢复均通过。不能据此宣称落地精度、机械转向精度、避障或语音识别准确率已验收。

测试分支 `codex/esp32-s3-voice-migration`；提交及合并以Git记录为准，本轮不推送远端。ESP应用SHA256：`a39f3f18d91251c55f53799c66fc5d7105f78708fefdbb4e4201c0a4f28164d4`，常驻AFE/WakeNet/VadNet，实验NS关闭。STM32本轮未重刷，上报build=`16086585dec0`、dirty=1、protocol=2；不能把这个dirty镜像称为当前仓库的干净构建。

接线：S3 GPIO4 TX→STM32 PA11 RX；S3 GPIO5 RX←STM32 PA12 TX；共地；115200/8N1。语音执行保持关闭，运动均由本轮显式回归脚本发出。

## 结果

| 项目 | 实测结果 |
|---|---|
| MQTT ping/pong | 多次成功；稳定窗口三次RTT 76.3 / 23.2 / 8.1ms |
| 静止可靠STOP×10 | 全通过，78.1–208.6ms；每次要求ACK接受及零速确认 |
| DIRECT前进/倒退 | ±30RPM，编码器方向正确，随后STOP通过 |
| DRIVE前进/倒退 | ±80mm/s，编码器方向正确，随后STOP通过 |
| 左/右小曲率 | 80mm/s、κ=-1/+1，内外轮速度差方向正确，随后STOP通过 |
| 单指令租约 | 不续租后自动停车，约836.9ms观察到lease_expired零速终态 |
| 直线GOAL | 目标5cm、5cm/s；报告4.6cm、1.448s、done、heading_source=imu；余差在现有到点容差内 |
| 运行中STOP | GOAL正确返回aborted_by_stop，STOP约207.3ms |
| 基础末尾STOP×10 | 全通过，70.6–137.9ms；基础脚本共11项通过 |
| 原地差速旋转拒绝 | 返回ACK status=3，保留舵机转向底盘约束 |
| 突发指令下STOP×3 | 每轮40条；STOP 440.6 / 503.2 / 468.8ms；确认后每轮8帧停车遥测，无再次启动 |
| S3重启会话×3 | 新session均变化；约3.49–3.51s恢复首个心跳（uptime=2s），随后STOP均确认；未下发运动 |
| 最终收尾 | STOP seq=52确认成功；连续4次idle、新鲜遥测、零速；IMU状态3 |

悬空距离是编码器轮转的等效距离，不是地面位移。转向只检查指令链及轮速分配，未量真实前轮角度。本轮不验收超声波；400cm读数只满足旧脚本的台架前置条件，不证明实物传感器安装或测距准确。

## 初期异常：不能隐去失败

1. 初始两次STOP调用返回504，未收到STM32 ACK，而上行数据仍可见。用户随后说明刚开始曾手动按STM32 RST两次；初期也出现IMU重新初始化/无数据，后来恢复。开始串口采集时另观察到一次S3重启。没有每次复位与每个超时逐帧关联的证据，不能武断认定唯一根因。之后稳定窗口的连续STOP、运动中STOP、突发STOP和S3重启恢复未再复现。仅有stm32_connected=true不证明下行可用。
2. 首次基础脚本在任何运动前遇到HTTP404，请求未进入pyserver访问日志。本机urllib受macOS系统代理影响；已让仅访问127.0.0.1的HIL脚本使用无代理opener，修正后同一API和完整回归通过。没有修改VPN或百度请求路由。
3. 重启脚本原来硬编码esp32c6，增加 `--chip esp32s3`，默认仍为C6。本轮只调整HIL脚本，没有为通过测试修改控制固件或放松停车确认。

## 证据与复用

本目录 `validation-s3-2026-11-11/` 保存基础回归、突发STOP、三次重启及最终状态。原始服务器和串口日志保留于本机 `/private/tmp/cmcar-s3-voice/`，不提交私人录音或凭据。

从CodeMyCar根目录，在确认硬件条件后执行：

```sh
pyserver/.venv/bin/python stm32_project/tests/hil/control_smoke.py --suspended --require-imu
pyserver/.venv/bin/python stm32_project/tests/hil/stop_burst_smoke.py --suspended --cycles 3
# 先关闭占用S3串口的monitor，再做仅重启/停车测试：
pyserver/.venv/bin/python stm32_project/tests/hil/session_restart_smoke.py --chip esp32s3 --port /dev/cu.usbserial-10 --esptool-python /Users/sunyameng/.espressif/tools/python/v6.1/venv/bin/python --cycles 3
```

端口可能随重插变化。脚本finally会尝试停车，但必须看结果确认，不能把“尝试”当成“成功”。`goal_heading_source_smoke.py`仅修了本机代理访问，本轮没有执行这个独立弧线/IMU差异用例。

后续：NS关闭后的真实语音准确率；正常联网条件下语音上传与控制并发压力；长时间重连/日志洪峰；落地精度。若无人复位时再出现无ACK，先保持停车并保存两板启动与双向时序，不以重启后恢复代替定位。
