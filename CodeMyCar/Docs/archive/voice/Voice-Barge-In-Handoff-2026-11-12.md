# 语音打断（Barge-in）实现与交接说明（2026-11-12）

**提交：** `4a0c280 feat(voice): device-side barge-in with AEC reference`  
**状态：** 主链路已实现并在实机验证到“可触发、可停止”；**参数标定与打断后的上下文
清理尚未完成**，还不能算稳定交付。  
**适用对象：** 接手 CodeMyCar 语音/AEC/打断开发的后续 agent。

---

## 1. 为什么之前做不了

打断需要区分“用户说话”和“喇叭回声”。起初只用电平门限，结果：

- 回声峰值 600–1000，和正常说话重叠，误打断严重；
- 播放期间不上传麦克风时，服务端看不到任何声音，无法打断；
- 播放期间上传麦克风时，回声被 ASR 当成用户问题，产生“乱七八糟的回复”。

根本原因是没有可靠的“回声 / 人声”判据。这块音频板的 **ES7210 ch1 是功放前
的模拟参考**，可以用来做真正的双讲检测。

---

## 2. 当前架构

```text
ES7210 TDM 4ch ── ch0 麦克风 ─┐
                 ch1 参考   ──┤
                              ▼
                    S3 AFE (MR, FD AEC + NLP)
                              │
                     AEC 后单声道 PCM
                              ├── VAD / WakeNet
                              └── 设备端双讲检测
                                     │ near-end
                                     ▼
                        flush 播放队列 + 丢弃在途 TTS
                                     │
                          {"type":"barge_in"} → pyserver
                                     │
                          pyserver 取消当前回合 + stop
```

关键点：

- **AFE 输入是 `MR` 交织**（mic, ref），AEC 使用 `AEC_MODE_FD_*`；
  只有 FD 模式 NLP 才生效（SR 模式打印 `NLP_OFF`）。
- **播放期间设备不上传麦克风**，回声永远不进 ASR。
- 打断判定在设备端完成，因为它同时拥有 mic 和 ref 的采样对齐数据。
- 服务端只负责取消回合、下发 `stop`、维护“打断后静音期”。

---

## 3. 设备端双讲检测

实现在 `components/voice_service/src/voice_service.c` 的会话循环中：

```c
if (正在播放) {
    out  = voice_frontend_out_rms();    // AEC 后电平
    corr = voice_frontend_echo_corr(); // mic 与 ref 的最佳相关（0..1000）
    ref  = voice_frontend_ref_rms();   // 参考通道电平
    near_end = ref > 2000 && out > 300 && corr < 350;
}
```

- 相关性由 `voice_frontend_s3.c` 在每个 32 ms feed 块上计算：
  `mic[i]` 与 `ref[i+lag]`，lag ∈ [-256, 256]（±16 ms），取最大 |corr|。
- 只在 `ref > 2000`（确实在播放）时计算，避免空闲时浪费 CPU 拖慢唤醒。
- `near_end` 持续累计 200 ms 触发；触发后 2.5 s 冷却。

> 当前 300/350 是待标定值。实测回声段 corr 约 0.38–0.73，人声段更低；
> 如果仍误触发，优先调整 corr 门限，而不是音量门限。

触发后：

1. `voice_output_flush()` 清空播放队列；
2. `voice_transport_barge_in()`：设置 `local_barge`（丢弃在途 TTS）并发送
   `{"type":"barge_in"}`；
3. 服务端取消当前 worker，发送 `{"type":"stop"}`；
4. 设备收到 `stop` 清除 `local_barge`，开始新的 `speak` 会话时恢复正常播放。

---

## 4. 服务端行为

`pyserver/app/services/conversation.py`：

- `device_barge_in()`：设备事件入口，取消当前回合；
- `_begin_barge_in()`：先取消 worker（避免 stop 排在整句 TTS 后面），
  再发 `stop`，清空队列，进入 `_post_stop`；
- `_post_stop`：打断后的音频全部丢弃，直到 **800 ms 连续静音**才恢复监听，
  防止“停一下”本身被当成新问题；
- `barge_in_device_events=True`：默认只信设备事件；旧的纯能量检测保留为
  fallback（`barge_in_device_events=False` 时才启用），因为实测它分不开回声。
- 打断后 2.5 s 冷却，避免回声反复触发。

协议：

- 设备 → 服务端：`{"type":"barge_in"}`
- 服务端 → 设备：`{"type":"stop"}`（设备清空播放）
- `start` 帧新增 `barge_in: true/false`（设备是否有 AEC 参考能力）

相关配置（`voice.env.example`）：

```text
CMCAR_VOICE_BARGE_IN=0/1
CMCAR_VOICE_BARGE_IN_RMS=700        # 仅 fallback 使用
CMCAR_VOICE_BARGE_IN_MS=200
```

---

## 5. 本次一并修复的问题

| 问题 | 修复 |
|---|---|
| 播放声音“滋啦” | 服务端按 0.8× 实时下发 + 设备 1.28 s 抖动缓冲（原来是欠载） |
| 长 TTS 中途断连 | WS 超时 5 s → 15 s；回调逐帧返回，不会长时间阻塞 |
| 服务端重启后设备卡在 listening | 15 s 死连接检测（仍可能最长 15 s） |
| 播放期丢帧导致会话中止 | 序列号重同步，不再断会话 |
| LLM 声称“车已经停稳了” | 语音执行关闭时不暴露车辆工具；`car_stop` 失败不再抛异常 |
| “停一下”被当车辆命令 | 停止词列表按包含匹配，直接结束当前回复，不进 LLM |
| 打断后立刻接话 | 打断后丢弃当前 utterance，静音 800 ms 后才听新问题 |

---

## 6. 已验证 / 未验证

**已验证（实机）：**

- 设备可识别“停一下”并本地停止播放；
- 服务端能收到 `barge_in` 并取消回合；
- 播放期间保持安静时不再频繁误打断（ref 门控后）；
- 唤醒、正常多轮对话、设备端 TTS 仍工作；
- pyserver 214 个测试通过。

**未完成：**

1. **DTD 阈值最终标定**：需要再跑 2–3 轮“安静 5 s → 停一下”，记录
   `dtd out/corr/ref`，确定 corr 与 out 门限；
2. **打断后的对话上下文**：上一回合被取消后，用户说“继续”可能接不上；
   需要在 agent session 里记录“本回合被用户打断”，或在新回合注入说明；
3. **自动化测试**：目前 barge-in 只有手工测试，没有回归用例；
4. 打断延迟仍有约 0.5–1 s（检测 200 ms + 网络 + DMA 尾音），未做量化；
5. 双麦 AFE（ch0/ch2）仍未接入。

---

## 7. 调试手段

设备串口（115200）：

- `s`：手动触发一次会话（诊断唤醒旁路）；
- `t`：本地播放 5 s 1 kHz 测试音（验证输出通路）；
- `a`：本地播放 5 s 噪声（验证 AEC/ERLE）。

日志关键字：

```text
voice_afe: aec in=.. out=.. ref=.. corr=.. erle=..
voice: dtd out=.. corr=.. ref=.. near_ms=..
voice: local barge-in corr=.. out=.. ref=..
voice: server stop: playback flushed
```

服务端：

```bash
cd pyserver
CMCAR_VOICE_LOCAL_PLAYBACK=0 CMCAR_VOICE_BARGE_IN=1 \
  .venv/bin/uvicorn app.main:app --host 0.0.0.0 --port 8000
```

---

## 8. 下一步建议（按优先级）

1. 用 `dtd` 日志完成阈值标定；若相关法仍不稳定，改用 AEC 双讲状态或
   参考-回声残差比，而不是继续调电平；
2. 处理“被打断回合”的会话历史：取消时给 agent runtime 打标记，避免“继续”
   接到错误上下文；
3. 为 `device_barge_in` / `_post_stop` / 停止词写单元测试与 WS 集成测试；
4. 量化打断延迟与误触发率（安静播放 10 分钟、正常对话 20 轮）；
5. 之后再考虑双麦 AFE 与更长滤波器/更高 NLP 的 AEC 优化。

---

## 9. 约束提醒

- 语音运动执行保持关闭（`CMCAR_VOICE_EXECUTE_COMMANDS=0`）；
- 所有运动仍必须经过 `AgentMotionService`/lease/可靠 STOP；
- 不 push、不改 main；本轮只提交到 `codex/s3-voice-architecture`；
- 电脑保持静音（`CMCAR_VOICE_LOCAL_PLAYBACK=0`）。
