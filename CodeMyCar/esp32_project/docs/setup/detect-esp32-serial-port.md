# 检测 ESP32 串口设备

> 适用平台：macOS (Apple Silicon)
> 目标芯片：ESP32-C6
> 实测环境：ESP-IDF v6.1.0 / esptool v5.6.0
> 记录日期：2026-10-25

烧录前必须先确认「哪个 `/dev/cu.*` 是目标板」。本文按「缩小范围 → 权威确认」三步走，最后给出可直接复制的脚本。

---

## TL;DR

```bash
get_idf                                             # 激活 IDF 环境（提供 esptool）
ls -l /dev/cu.*                                     # 1. 列候选
ioreg -p IOUSB -w0 -l | grep -iE "USB .*Name|IOCalloutDevice"   # 2. 看厂商
esptool --port /dev/cu.usbmodem101 --connect-attempts 1 chip-id # 3. 握手确认
```

只有第 3 步返回 `Connected to ESP32-...`（rc=0）的口才是 ESP 设备。

---

## 第 1 步：列出候选串口

```bash
ls -l /dev/cu.*
```

典型输出：

```
crw-rw-rw-  /dev/cu.Bluetooth-Incoming-Port   ← 系统蓝牙，排除
crw-rw-rw-  /dev/cu.debug-console             ← 系统调试口，排除
crw-rw-rw-  /dev/cu.usbmodem101               ← 候选
crw-rw-rw-  /dev/cu.usbmodem2020_12_222       ← 候选
```

要点：

- macOS 上 USB CDC 设备统一显示为 `usbmodem*`。
- `tty.*` 和 `cu.*` 是同一设备的两条路径：**烧录/监控用 `cu.*`**（不会被登录进程/流控占用阻塞）。
- 顺带检查端口是否被占用（比如另一个终端正在 `idf.py monitor`）：

```bash
lsof /dev/cu.*        # 有输出说明被某个进程占用，先关掉再烧
```

## 第 2 步：从 USB 描述符看厂商（线索，不是结论）

```bash
ioreg -p IOUSB -w0 -l | grep -iE "USB Product Name|USB Vendor Name|IOCalloutDevice"
```

ESP32-C6 原生 USB（USB-Serial/JTAG）会显示：

```
USB Vendor Name  = Espressif
USB Product Name = USB JTAG/serial debug unit
IOCalloutDevice  = /dev/cu.usbmodem101
```

其他设备则显示自己的名字，例如：

```
USB Product Name = LDR2001
IOCalloutDevice  = /dev/cu.usbmodem2020_12_222
```

⚠️ **这一步只能排除明显不对的设备，不能作为结论**：

- 用 CP210x / CH340 等 USB-UART 桥的 ESP 板，厂商名会是 `Silicon Labs` / `QinHeng`，不含 "Espressif"；
- 反过来，ESP32-C3 / S3 / H2 也显示同样的 `USB JTAG/serial debug unit`。

## 第 3 步：跟 ROM bootloader 握手（权威判定）

真正向串口发送 esptool 同步序列，只有能进入 ROM bootloader 的芯片才会应答：

```bash
get_idf
esptool --port /dev/cu.usbmodem101 --connect-attempts 1 chip-id
```

成功（rc=0）：

```
esptool v5.6.0
Serial port /dev/cu.usbmodem101:
Connecting....
Detecting chip type... ESP32-C6
Connected to ESP32-C6 on /dev/cu.usbmodem101:
Chip type:          ESP32-C6 (QFN40) (revision v0.1)
Features:           Wi-Fi 6, BT 5 (LE), IEEE802.15.4, Single Core + LP Core, 160MHz
USB mode:           USB-Serial/JTAG
BASE MAC:           40:4c:ca:4b:dc:d8
...
Hard resetting via RTS pin...
```

失败（rc=2，说明该口不是 ESP 设备）：

```
ERROR: A fatal error occurred: Failed to connect to Espressif device: No serial data received.
```

细节：

- esptool **v5** 的子命令是 `chip-id`（带连字符）；v4 时代写作 `chip_id`。看版本：`esptool version`。
- `--connect-attempts 1` 让失败快速返回，否则默认会重试多轮较慢。
- 这条命令会**硬复位芯片**，正在运行的固件会重启。
- ESP32-C6 没有传统 chip ID，esptool 会打印提示并改读 MAC；`BASE MAC` 就是这块板子的稳定标识，可用于区分多块同型号板子。
- 其他可选命令：`read-mac`（只读 MAC）、`flash-id`（读 Flash 厂商/型号/容量）。

---

## 一键扫描所有候选口

```bash
get_idf
for p in $(find /dev -maxdepth 1 -name 'cu.usbmodem*' 2>/dev/null | sort); do
  echo "== $p"
  esptool --port "$p" --connect-attempts 1 chip-id 2>&1 \
    | grep -E "Connected to|Failed to connect|No serial data"
done
```

实测输出：

```
== /dev/cu.usbmodem101
Connected to ESP32-C6 on /dev/cu.usbmodem101:
== /dev/cu.usbmodem2020_12_222
ERROR: A fatal error occurred: Failed to connect to Espressif device: No serial data received.
```

> 这里用 `find` 而不是 `/dev/cu.usbmodem*` 通配符：若 shell 启用了 `NO_BARE_GLOB_QUAL` / `NO_NOMATCH` 等选项，`*(N)` 之类的 glob 可能直接报 `no matches found` 中断脚本。

---

## 判定速查

| 现象 | 结论 / 处理 |
|---|---|
| 只有一个 `usbmodem*` 且 `chip-id` 通过 | 直接用它烧录 |
| 多个口，`ioreg` 显示 Espressif | 再逐个跑 `chip-id` 确认，取通过的那个 |
| `chip-id` 报 `No serial data received` | 不是 ESP，或板子未上电/线是纯充电线/需要按住 BOOT |
| `ioreg` 里厂商是 Silicon Labs / QinHeng | 板子走 USB-UART 桥，仍可能是 ESP，必须用 `chip-id` 确认 |
| `lsof /dev/cu.xxx` 有输出 | 端口被占用（monitor 等），先关闭占用进程 |
| `esptool: command not found` | 未激活环境，先 `get_idf`（见 [dev-workflow.md](dev-workflow.md)） |
| `Permission denied` 打开串口 | macOS 一般不需要 sudo；检查是否被其他程序独占 |
| 板子反复连不上 | 手动进下载模式：按住 **BOOT** → 点按 **RST** → 松开 BOOT，再执行 |

---

## 确认端口之后

```bash
get_idf
idf.py -p /dev/cu.usbmodem101 flash          # 烧录（会自动增量编译）
idf.py -p /dev/cu.usbmodem101 flash monitor  # 烧完直接看日志，Ctrl+] 退出
```

> ⚠️ 若刚从旧版本 ESP-IDF 升级，`build/` 里仍引用旧版本路径，**先 `idf.py fullclean`** 再 build/flash。

---

## 其他平台对照

| 平台 | 设备节点 |
|---|---|
| macOS | `/dev/cu.usbmodem*`（原生 USB）、`/dev/cu.usbserial-*` / `/dev/cu.wchusbserial*`（USB-UART 桥） |
| Linux | `/dev/ttyACM*`（CDC）、`/dev/ttyUSB*`（USB-UART 桥）；`lsusb` 查厂商 |
| Windows | `COMx`；设备管理器查看，或 `esptool --port COMx chip-id` |

跨平台一致的做法都是第 3 步的 `esptool chip-id`。

---

## 附：本机实测记录（2026-10-25）

```
/dev/cu.usbmodem101
  USB Vendor  = Espressif
  USB Product = USB JTAG/serial debug unit
  chip-id     = ESP32-C6 (QFN40) rev v0.1, USB-Serial/JTAG
  BASE MAC    = 40:4c:ca:4b:dc:d8
  → ✅ 目标板

/dev/cu.usbmodem2020_12_222
  USB Product = LDR2001（非 Espressif）
  chip-id     = Failed to connect: No serial data received
  → ❌ 不是 ESP 设备
```
