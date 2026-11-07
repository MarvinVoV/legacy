# BNO085（GT-BN008X）台架测试

独立的 ESP32 测试工程，只用来验证九轴传感器，**与小车 `esp32_project` 无关**。
测完后把小车 bridge 固件刷回即可。

## 接线（I²C 模式，ESP32-C6 3.3V，无需电平转换）

| 传感器引脚 | ESP32-C6 | 说明 |
|---|---|---|
| `VCC_3V3` | **3V3** | 3.3V 供电，约 10 mA |
| `GND` | **GND** | |
| `SDA` | **GPIO6** | 代码里 `I2C_SDA` |
| `SCL` | **GPIO7** | 代码里 `I2C_SCL` |
| `CS` | **3V3** | I²C 模式必须为高 |
| `PS0` | **GND** | |
| `PS1` | **GND** | PS0/PS1 都低 = I²C |
| `AD0` | GND 或 3V3 | 决定地址 0x4A / 0x4B，程序两个都试 |
| `RST` | 悬空 | 模块内部有上拉 |
| `INT` | 悬空 | 程序用轮询 |

> GPIO4/5 被小车的 UART 占用，所以选 6/7。若你的板子丝印上 6/7 不可用，
> 换任意两个空闲 GPIO，并同步改 sketch 顶部的 `I2C_SDA` / `I2C_SCL`。

## 编译 / 烧录 / 观察

```bash
# 编译
arduino-cli compile --fqbn esp32:esp32:esp32c6 bno085_test

# 插上 ESP32（CP2102 口）后
arduino-cli board list                     # 确认端口，如 /dev/cu.usbserial-10
arduino-cli upload -p /dev/cu.usbserial-10 --fqbn esp32:esp32:esp32c6 bno085_test
arduino-cli monitor -p /dev/cu.usbserial-10 -c baudrate=115200
```

## 预期输出

```
I2C scan:
  found 0x4A
BNO085 ready
yaw=    0.00 deg  gyroZ=   0.01 dps
yaw=    0.12 deg  gyroZ=   0.00 dps
```

- 转动模块：`yaw` 平滑变化（右手定则，绕 Z 轴）
- 静止：`yaw` 应稳定（game rotation vector 不依赖磁力计，不受电机干扰）

## 故障排查

| 现象 | 处理 |
|---|---|
| I²C 扫描为空 | 查 CS 是否接 3V3、PS0/PS1 是否接 GND、3.3V 是否到位 |
| 扫到地址但 init 失败 | 给 RST 加复位：接 GPIO，上电拉低 10 ms 再拉高 |
| yaw 数值不动 | 确认库版本（Adafruit BNO08x）；重新上电 |
| 长时间 yaw 漂移 | 正常（陀螺积分），静止时做零偏标定 |

## 测完恢复小车固件

```bash
cd CodeMyCar/esp32_project/build
/Users/sunyameng/.espressif/tools/python/v6.1/venv/bin/esptool --chip esp32c6 \
  -p /dev/cu.usbserial-10 -b 460800 \
  --before default-reset --after hard-reset \
  write-flash --flash-mode dio --flash-freq 80m --flash-size 8MB \
  0x0 bootloader/bootloader.bin 0x8000 partition_table/partition-table.bin \
  0x10000 esp32_project.bin
```
