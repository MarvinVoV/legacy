# Phase 11：TRACE-01 固件构建身份修复验证

日期：2026-11-01  
分支：`codex/fix-remaining-runtime-safety`

## 1. 修复内容

- ESP32 CMake 将 12 位 Git SHA 和 dirty 状态写入 app descriptor；
- STM32 CMake 将相同信息及协议版本写入固件；
- ESP32 每次启动发送 SESSION 后，STM32 立即回传 `BUILD_INFO(0x09)`；
- ESP32 保存该身份，并在 heartbeat 中同时发布 ESP32 build、ESP-IDF 版本、STM32 build、STM32 dirty 和串口协议版本；
- 无 Git 环境时明确回退为 `unknown/dirty`，不会伪造 clean 身份。

## 2. 构建与刷写纪律

TRACE-01 的初始实现提交为 `2f7f84649e08`。全部后续修复完成后，又从正式代码提交 `5d586f709b20` 强制重新配置、构建并刷写两块板：

- ESP32 app descriptor：`5d586f709b20`，无 `-dirty`；
- STM32 ELF 字符串：`5d586f709b20`、`CMCAR_GIT_DIRTY 0`、协议 v2；
- ESP32 使用 `--flash-size 8MB` 烧录并完成 hash verify；
- STM32 download、verify、reset 成功。

本验证文档是在固件构建后新增，因此不会改变板上固件所报告的代码提交；`5d586f7` 是包含全部产品代码修复和 UART 压测脚本的精确提交，之后的提交仅包含验证/交接文档。

## 3. 端到端证据

复位后首个 MQTT heartbeat：

```json
{
  "stm32_connected": true,
  "esp_build": "5d586f709b20",
  "esp_idf": "v6.1",
  "stm32_build": "5d586f709b20",
  "stm32_dirty": false,
  "protocol_version": 2
}
```

这同时验证了 ESP32 app descriptor、ESP32→STM32 SESSION、STM32→ESP32 BUILD_INFO、UART 解码和 MQTT heartbeat 整条路径。

## 4. 结论

`TRACE-01` 已关闭。运行时无需连接调试器即可把两块板上的固件精确映射到 Git 提交，并能识别 STM32 dirty 构建与协议版本不匹配。
