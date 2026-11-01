# CFG-01：ESP32 8MB Flash 配置修复验证

日期：2026-11-01  
分支：`codex/fix-remaining-runtime-safety`

## 1. 问题

ESP32-C6 实际安装 8MB SPI Flash，但项目 `sdkconfig` 和生成镜像头声明 2MB。构建与刷写日志因此长期出现硬件容量和固件配置不一致。

## 2. 实际容量确认

通过板卡串口执行 esptool `flash-id`，结果：

```text
Manufacturer: c8
Device: 4017
Detected flash size: 8MB
```

因此本板应配置为 **8MB**，不是 2MB 或 4MB。

## 3. 修复

- `sdkconfig.defaults` 新增 `CONFIG_ESPTOOLPY_FLASHSIZE_8MB=y`，保证重新生成配置时仍采用 8MB；
- `sdkconfig` 从 2MB 改为 8MB；
- ESP-IDF v6.1 根据 8MB 目标自动将 ESP32-C6 MMU page 从 32KB 调整为 64KB；
- 保留 `CONFIG_PARTITION_TABLE_SINGLE_APP_LARGE=y`，本次只修复物理容量/镜像头不一致，不擅自引入 OTA 分区迁移。

## 4. 验证

### 4.1 构建

ESP-IDF v6.1 完整重构建成功：

```text
esp32_project.bin binary size 0xf9590 bytes
Smallest app partition is 0x177000 bytes
0x7da70 bytes (34%) free
```

生成的推荐刷写命令明确为：

```text
--flash-size 8MB
```

### 4.2 镜像头

`esptool image-info build/esp32_project.bin`：

```text
Flash size: 8MB
Flash freq: 80m
Flash mode: DIO
Chip ID: 13 (ESP32-C6)
```

### 4.3 刷写与运行

- esptool 使用 `--flash-size 8MB` 刷写；
- bootloader 和应用 hash verify 成功；
- 复位后 heartbeat 正常：`stm32_connected=true`；
- 新 session 为 `3042605223`；
- 最终可靠 STOP：`seq=1, accepted=true, confirmed=true`。

## 5. 分区边界

“Flash 配置 8MB”和“把 8MB 全部分区使用”是两件事。

当前应用约 998KB，现有 1.5MB app 分区仍有 34% 余量，继续使用 single-app-large 不影响运行，也避免无需求依据地改变 NVS/应用布局。后续如果确定需要 OTA，建议单独设计 8MB 自定义分区表，例如双 OTA slot、NVS 和可选数据区，并安排完整擦写与回滚测试；不应把 OTA 布局变更混入本配置修复。

## 6. 结论

`CFG-01` 已完成实物容量读取、配置修改、完整构建、镜像头检查、刷写和运行验证，可以关闭。
