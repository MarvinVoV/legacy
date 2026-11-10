# Voice Long-Conversation Mode Design

**Status:** Draft for approval. No production code in this document has been written yet.

**Goal:** after one wake word, keep a natural voice conversation open for multiple turns: the user speaks several sentences, a pause ends the current turn, DeepSeek replies, Baidu TTS speaks through the car, and the session ends after a long silence or an explicit end phrase. The existing one-shot command mode remains the default and is not changed.

**Priority change (2026-11-11):** barge-in / interruption is **phase 2**. Phase 1 delivers the long conversation with half-duplex turn-taking. The protocol reserves barge-in events, but phase 1 does not claim interruption.

---

## 1. Why

The current path is optimized for one command per wake:

- S3 `voice_service.c` opens a session, uploads one utterance, waits for `final`, then closes.
- pyserver `VoiceSessionRunner(one_shot=True)` stops after the first VAD segment.
- `voice_intent.py` only accepts exact whitelisted commands.

That is correct for safe motion commands but wrong for the primary use case: talking with an LLM. The new mode must not disturb the command path or its safety properties.

## 2. Goals

- One wake word starts a conversation; subsequent turns do not require the wake word.
- Multiple user turns in one WebSocket session.
- Pause-based turn end: **700 ms** of silence after speech.
- Session ends after **15 s** with no effective user speech, or an explicit end phrase (`结束`, `再见`, `退出`, `不聊了`).
- ASR text goes to DeepSeek; streamed reply text is spoken through Baidu TTS.
- First spoken response should start as early as possible: stream LLM content, split at sentence boundaries, synthesize and send each sentence without waiting for the full answer.
- Conversation history is kept for the session, with a token/turn budget; no persistence.
- Vehicle control is an LLM tool, not the main path; execution remains disabled by default and goes through the existing AgentMotionService/lease/reliable STOP boundary.
- Mode selection is explicit and isolated: `command` (current) and `conversation` (new).
- If the LLM, ASR, TTS or WebSocket fails, the session ends cleanly; the car returns to idle and the control bridge keeps running.
- Phase 1 is half-duplex: while TTS is playing, user audio is drained, not recognized. The assistant never transcribes its own TTS.

## 3. Non-goals

- No barge-in in phase 1 (phase 2; requires AEC/reference).
- No always-on microphone: wake word still gates the session.
- No multi-command motion execution. One LLM tool call represents one motion request; no concurrent moves.
- No free-form control of arbitrary GPIO/UART.
- No server-side persistence of conversations or audio archives by default.
- No Opus/protocol format change in phase 1; PCM 16 kHz mono remains.

## 4. Anti-goals

- A conversation mode that silently changes command-mode behavior.
- Letting the LLM call motion tools without the existing execution gate and safety limits.
- Treating TTS echo as user speech (phase 1 avoids by draining during playback).
- Blocking the audio capture task on the LLM/TTS/network path.
- A "long conversation" implemented by repeatedly reconnecting and replaying history from scratch on every turn.
- Logging API keys, raw user audio, or full secrets.

## 5. Approved product decisions

| Decision | Value |
|---|---|
| Turn-end silence | 700 ms |
| Session idle end | 15 s without effective user speech |
| Wake word required after first turn | No; only at session start |
| End phrase | `结束`, `再见`, `退出`, `不聊了` (exact normalized match) |
| Default LLM | `deepseek-flash`, base `https://api.deepseek.com` |
| Thinking | disabled by default for voice latency |
| TTS | Baidu, existing premium voice `per=5`, 16 kHz mono PCM |
| Playback phase 1 | current Mac player; device amplifier when the new board/codec output exists |
| Barge-in | phase 2, not required for phase-1 acceptance |
| Command mode | unchanged, still default |

## 6. Architecture at a glance

```
S3 microphone
  -> voice_audio (I2S/AFE, VAD, bounded PCM queue)
  -> voice_conversation.c (new; owns session loop and state)
  -> voice_transport (WebSocket PCM up, speak/events down)
  -> pyserver /api/v1/voice/stream (mode=conversation)
       -> ConversationSession (VAD, turn detection, history)
       -> Baidu ASR
       -> DeepSeekChatClient (streaming, tools)
       -> SentenceChunker -> Baidu TTS -> speak frames + PCM
  -> S3 voice_output (queue; current no-op, future amp codec)
  -> Mac player fallback (phase 1)
```

The command path remains:

```
wake -> one VAD segment -> ASR -> VoiceIntent -> VoiceActionExecutor -> final -> end
```

## 7. Mode selection and isolation

### 7.1 S3

- New Kconfig choice `CMCAR_VOICE_MODE`:
  - `CMCAR_VOICE_MODE_COMMAND` (default, current behavior)
  - `CMCAR_VOICE_MODE_CONVERSATION`
- `voice_service.c` keeps the existing `voice_task`/`run_session` command path untouched.
- New `voice_conversation.c/.h` owns the conversation loop and its own state enum. It must not call command-mode `run_session`.
- `voice_service_start()` dispatches once at startup; no runtime mode mixing in phase 1.
- Existing `voice_audio`, `voice_frontend_s3`, `voice_wake`, codec and transport remain shared infrastructure.

### 7.2 pyserver

- `POST`/WebSocket start frame gains `"mode": "command" | "conversation"`. Missing mode defaults to `command`, so older firmware is unchanged.
- Route selects `VoiceSessionRunner` (command) or `ConversationRunner` (new). They must not share mutable session state.
- New modules:
  - `app/services/llm.py` — provider-neutral `LlmClient` + `DeepSeekLlmClient`.
  - `app/services/conversation.py` — `ConversationRunner`, `ConversationSession`, turn state, history.
  - `app/services/conversation_tts.py` — sentence chunker and ordered TTS stream.
  - `app/api/routes/voice.py` — mode dispatch only.
- Existing `voice_pipeline.py`, `voice_intent.py`, `VoiceActionExecutor` stay for command mode and LLM tools reuse the action executor boundary.

## 8. Wire protocol

Use the existing `/api/v1/voice/stream` endpoint. Text frames are JSON, binary frames are PCM.

### 8.1 Start

```json
{
  "type": "start",
  "device": "DEV_1",
  "rate": 16000,
  "ch": 1,
  "bits": 16,
  "mode": "conversation",
  "barge_in": false
}
```

`barge_in` is reserved for phase 2. Phase 1 conversation ignores it and operates half-duplex.

### 8.2 Server events

```json
{"type":"ready","mode":"conversation","session":"...","rate":16000}
{"type":"user_text","turn":1,"text":"...","intent":null}
{"type":"assistant_delta","turn":1,"text":"..."}          // optional UI/log mirror
{"type":"speak","turn":1,"seq":1,"text":"...","format":"pcm","rate":16000,"channels":1,"bytes":32000}
<binary PCM>
{"type":"assistant_done","turn":1,"text":"...","interrupted":false}
{"type":"interrupted","turn":1}                           // phase 2 only
{"type":"end","reason":"idle|user_end|error|max_duration|max_turns"}
{"type":"error","message":"..."}
```

The existing `speak` parser in `voice_protocol/voice_transport` must accept optional `turn`/`seq` fields; it already validates `rate`, `channels`, `bytes`.

### 8.3 Client events

```json
{"type":"end"}                                           // user/OS ends session
{"type":"barge_in","turn":1}                             // phase 2
```

### 8.4 Ordering rules

- All server sends go through `WebSocketEventSink`'s lock.
- `speak` and its binary PCM are sent as one ordered unit.
- `assistant_done` follows the last PCM of the turn.
- `end` is sent after all turn tasks are cancelled/joined.
- After `end`, the server sends nothing on that connection.

## 9. pyserver conversation pipeline

### 9.1 Session lifecycle

`ConversationSession` states:

`LISTENING -> USER_SEGMENT -> THINKING -> SPEAKING -> LISTENING -> ... -> ENDING`

- `LISTENING`: VAD receives PCM. `end_silence_ms=700`, `min_speech_ms=200`, `pre_roll_ms=400`, `max_segment_ms=10000`.
- `USER_SEGMENT`: a complete segment is queued for ASR. If the client is silent for 15 s while `LISTENING`, send `end(reason=idle)`.
- `THINKING`: ASR and LLM/tool loop. No new user segment is processed in phase 1 (half-duplex).
- `SPEAKING`: TTS frames are streamed. PCM from the client is drained, not detected. When playback finishes and `assistant_done` is sent, return to `LISTENING`.
- `ENDING`: cancel tasks, join, send `end`, close.

The session has `max_duration_s=600`, `max_turns=30`, and a context budget (see 9.3).

### 9.2 Turn flow

1. VAD produces a segment.
2. ASR (`BaiduAsrClient`) returns text. Empty/error:
   - send `error` with a short message, remain listening; do not append to LLM history.
3. Append `{"role":"user","content":text}`; send `user_text`.
4. Run the LLM/tool loop:
   - Call DeepSeek streaming with `thinking` disabled.
   - Accumulate `content`; collect `tool_calls`.
   - If tool calls: execute each through `CarToolExecutor` (AgentMotionService), append `role=tool` results, and call the LLM again (max 3 tool rounds).
   - Speech is emitted only for final answer content, not for intermediate "let me check" text. If an assistant chunk has tool calls and content, buffer it until the tool loop finishes; then speak the final content.
5. Sentence chunker splits content and pushes sentence jobs to an ordered TTS worker. Each job:
   - `BaiduTtsClient.synthesize(sentence, aue=4)`
   - send `speak` header with `turn`/`seq`/`text`/`rate`/`channels`/`bytes`
   - send binary PCM
   - optional `play_local` fallback in phase 1
6. On completion: send `assistant_done`, append assistant content to history, clear turn state, return to `LISTENING`.
7. On error/timeout: send `error`; if a partial answer was spoken, mark `assistant_done(interrupted=true)` with the spoken text; return to `LISTENING` or end according to policy.

### 9.3 Context

- System prompt: car assistant, concise spoken Chinese/English, no markdown, no code blocks, ask before risky actions, never claim a motion succeeded unless the tool result says so.
- History: system + completed user/assistant/tool messages.
- Budget: keep the last `max_history_turns=12` turns and at most `max_history_tokens=8000` (conservative estimate); drop oldest user/assistant pairs first; never drop the system prompt.
- Thinking disabled: do not store/return `reasoning_content`.
- If a later version enables thinking with tools, all subsequent requests must return each turn's `reasoning_content`; that is deliberately out of phase 1.

### 9.4 LLM client contract

```python
class LlmClient(Protocol):
    async def stream_chat(
        self,
        messages: list[dict],
        *,
        tools: list[dict] | None = None,
        cancel: asyncio.Event | None = None,
    ) -> AsyncIterator[LlmDelta]: ...
```

`DeepSeekLlmClient`:
- `POST https://api.deepseek.com/chat/completions`
- `stream=True`
- `model=deepseek-flash` by default; `deepseek-v4-pro` selectable by config
- `thinking={"type":"disabled"}` by default
- parse SSE `delta.content`, `delta.tool_calls`, `finish_reason`
- timeout: connect 10 s, first token 20 s, total 60 s
- no key in logs/errors; key only from `pyserver/voice.local.env`

### 9.5 TTS streaming and ordering

- Sentence boundaries: Chinese `。！？；…` and English `.?!;` plus newline.
- Minimum chunk length 6 chars; maximum 80 chars; flush remaining at end.
- One ordered TTS worker per turn; `seq` increments.
- Do not start a sentence until it is complete; do not wait for the full LLM answer.
- TTS failure: send `error`, stop the turn, keep the session alive (or end after repeated failures).
- Phase 1 half-duplex: while the TTS worker is active, VAD detection is disabled and incoming PCM is drained. This avoids the assistant transcribing its own speech. The earlier ready-tone bug showed why prompt tones must not be conflated with TTS gating; phase 1 uses an explicit `SPEAKING` state.
- Phase 2 will replace the drain with AEC + `barge_in` handling.

### 9.6 Session end

- Exact user text after normalization in `{结束, 再见, 退出, 不聊了}`:
  - optionally speak a short "好，再见" through TTS;
  - send `assistant_done` then `end(reason=user_end)`.
- 15 s with no effective user speech after the last `assistant_done`: `end(reason=idle)`.
- 600 s or 30 turns: `end(reason=max_duration|max_turns)`.
- Client `end`, WS close, server shutdown: cancel LLM/TTS tasks, join, close, release session.
- No automatic motion from session end. Motion safety remains in AgentMotionService; a voice session ending must not leave a voice-issued move silently running. If a motion tool is active, its normal lease/STOP path applies; phase 1 does not add a second controller.

## 10. S3 conversation path

### 10.1 New session loop

`voice_conversation.c` (new):

1. Set state `CONVERSATION_CONNECTING`.
2. `voice_transport_open(..., mode=conversation, barge_in=false)`.
3. Flush captured audio; wait 250 ms; flush again (drop wake-word tail).
4. State `CONVERSATION_LISTENING`.
5. Loop while session active:
   - Receive a 20 ms `voice_frame_t`; send PCM via `voice_transport_send_pcm`.
   - Poll `voice_transport_events`:
     - `SPEAK`: binary PCM is fed to `voice_output` (current no-op).
     - `ASSISTANT_DONE`: state back to listening.
     - `END`/`FAILED`/`DISCONNECTED`: break.
   - If no frame for 500 ms: error/abort.
   - No `VOICE_PEER_FINAL` break; conversation ends only on server `end`.
6. On exit: `voice_transport_close`, `voice_output_stop`, cooldown, flush, reset wake, idle.

### 10.2 State isolation

New enum in `voice_state.h` or a separate `voice_conversation_state.h`:

`CONVERSATION_CONNECTING/LISTENING/THINKING/SPEAKING/COOLDOWN`

The command state machine (`VOICE_STARTING/IDLE/...`) is not reused for conversation transitions, so command tests and logs remain stable.

### 10.3 Events and output

- `voice_protocol` adds `VOICE_PEER_USER_TEXT`, `VOICE_PEER_ASSISTANT_DELTA`, `VOICE_PEER_ASSISTANT_DONE`, `VOICE_PEER_INTERRUPTED` bits (phase 2).
- `voice_transport` gets `voice_transport_send_json` (or `send_end`/`send_barge_in`) and exposes `speak` metadata (`turn`, `seq`) without blocking the audio task.
- `voice_output` gets `voice_output_stop()` (clear queue, stop codec write) for phase 2 and session teardown. In phase 1 it is called on end/error.
- The current output queue already drops the TTS tail if full; conversation must not block capture on output.

### 10.4 Half-duplex phase 1

- The S3 still uploads audio during `SPEAKING`; the server drains it (detect disabled). This keeps the connection and VAD state simple, but no user turn is recognized until `assistant_done`.
- The S3 keeps its microphone/queue running; no reconnection.
- Phase 2 (not acceptance for phase 1): AEC/reference, client `barge_in`, server cancellation, output stop.

## 11. Vehicle tools and safety

Tool schemas (OpenAI function format):

- `car_move(direction: forward|backward, distance_m: number, turn_deg?: number)` — maps to `VoiceActionExecutor`/AgentMotionService; limits from `VoiceActionConfig`; rejects out-of-range.
- `car_stop()` — reliable stop via AgentMotionService.
- `car_status()` — read-only snapshot.

Rules:

- `voice_execute_commands=0` by default: tools return `{"executed": false, "reason": "voice execution disabled"}`; the model must say it cannot move.
- When enabled, every call goes through the existing executor; no direct UART/MQTT/motor access from the LLM.
- `car_stop` is never blocked by TTS/LLM tasks; a stop request may cancel the current speech turn.
- One motion tool at a time; no queueing multiple moves from one answer; reject concurrent.
- The model may propose but not execute if execution is disabled.
- No secrets or raw sensor data beyond the tool result.

## 12. Configuration and secrets

`pyserver/voice.local.env` (gitignored, mode 0600):

```text
CMCAR_LLM_API_KEY=...
CMCAR_LLM_BASE_URL=https://api.deepseek.com
CMCAR_LLM_MODEL=deepseek-flash
CMCAR_LLM_MODEL_PRO=deepseek-v4-pro
```

New app settings (defaults):

```text
voice_conversation_enabled=true
voice_conversation_turn_end_ms=700
voice_conversation_idle_s=15
voice_conversation_max_s=600
voice_conversation_max_turns=30
voice_conversation_history_turns=12
voice_conversation_history_tokens=8000
voice_llm_model=deepseek-flash
voice_llm_thinking=disabled
voice_llm_max_tokens=512
voice_barge_in=false          # phase 2
```

S3 Kconfig:

```text
CMCAR_VOICE_MODE_COMMAND (default)
CMCAR_VOICE_MODE_CONVERSATION
CMCAR_VOICE_BARGE_IN (phase 2; default n)
CMCAR_VOICE_AEC_MODE_OFF/SOFTWARE_REF/HARDWARE_REF (phase 2; default off)
```

Never print the key; never include it in logs, docs, commits, or test fixtures.

## 13. Failure and timeout behavior

| Failure | Behavior |
|---|---|
| ASR timeout/error | `error` event; stay listening; no LLM call |
| LLM first token timeout | speak a short "网络有点问题"; end turn; stay listening |
| LLM mid-stream error | speak partial or fallback; `assistant_done(interrupted=true)`; stay listening |
| Tool error/timeout | tool result with error; let LLM explain; no motion claim |
| TTS failure | `error`; stop current turn; stay listening |
| WS disconnect | cancel all tasks; no `end` delivery; release session |
| S3 audio producer stalled | abort session; close; cooldown; idle |
| Server shutdown | cancel tasks; close; no orphan playback |
| Session idle | `end(idle)`; S3 idle; wake again |

All cancellation must be idempotent; no task may outlive its session; no audio frame may be sent after `end`.

## 14. Testing and acceptance

### Unit / host

- Mode routing: command without `mode` is unchanged.
- VAD turn detection: 700 ms pause, pre-roll, min speech, 10 s cap.
- Session idle (15 s), max duration/turns, end phrases.
- LLM SSE parser: content deltas, tool calls, errors, timeout, cancellation.
- History trimming: turn/token budget; system prompt retained.
- Sentence chunker: Chinese/English punctuation, long sentence, no empty chunks.
- Tool loop: disabled execution, range rejection, stop, status, max tool rounds.
- TTS ordering: `speak` headers `turn`/`seq`; binary follows header; no interleaving; cancel drops tail.
- Half-duplex: PCM during `SPEAKING` is drained, not ASR'd.
- Regression: existing pyserver suite (199) and voice host tests remain green.

### Live acceptance (phase 1)

1. One wake, then 5+ turns without repeating the wake word.
2. 700 ms pause ends a turn; the next assistant sentence starts without a new wake.
3. 15 s silence ends the session; S3 returns idle; wake works again.
4. `再见` ends with a short closing and idle.
5. ASR text and LLM answer are coherent over 5 turns (context retained).
6. First TTS audio starts while the LLM is still generating (streaming evidence in logs).
7. During TTS, user speech is not transcribed; after `assistant_done`, the next turn is recognized.
8. No motion unless execution is explicitly enabled; with execution off, tools report disabled.
9. Network/LLM/TTS failure ends or recovers without resetting the control bridge.
10. 10-minute idle memory/stack check; no leaked tasks; no orphan child processes.

### Phase 2 (not acceptance for phase 1)

- AEC reference from the new board (hardware) or outgoing TTS PCM (software) cancels playback echo.
- `barge_in` cancels LLM/TTS within 300 ms; output queue flush; next user turn is recognized.
- AEC on/off A/B; false barge-in rate; current Mac path documented as half-duplex.

## 15. Implementation boundaries / open items

- New board codec output/reference is not available yet. Phase 1 keeps Mac playback and `voice_output` no-op; the design does not claim device amp playback until the board codec is implemented.
- New board notes (2026-11-11): **ES7210 is a 4-channel I2S ADC** (dual microphone input), while **NS4150B is an analog-input 3 W class-D amplifier, not a DAC**. A DAC/codec between the ESP32-S3 I2S output and NS4150B is still required; the exact board DAC, I2S pins, MCLK, mic channel mapping and any playback-reference loopback must come from the board schematic. Do not guess these.
- Barge-in is phase 2; the wire fields are reserved but `barge_in=false` in phase 1. For phase 2, prefer a hardware reference loopback into an ES7210 spare ADC channel; otherwise feed outgoing TTS PCM as a software reference and validate AEC before claiming barge-in.
- Dual-microphone ES7210 beamforming/SE is also out of phase 1; start with one microphone channel so the AFE pipeline stays comparable to the current one-mic setup.
- LLM key is in local env only; not in git.
- `deepseek-flash` is the default; `deepseek-v4-pro` is selectable; thinking is disabled for voice.
- A future Opus/format negotiation is out of scope.
- A future multi-command motion parser is out of scope; one tool call = one safe motion request.

## 16. Acceptance gate

Phase 1 is complete when the live acceptance above is recorded with logs and the command-mode regression suite is green. Barge-in remains a separate phase-2 deliverable; do not mark it complete.

## 17. Implementation notes after builder review (2026-11-11)

Phase 1 is implemented as `conversation` mode; `command` mode remains untouched.

- **`assistant_done` waits for playback.** In phase 1 the Mac player is the
  audible output. The runner calls `wait_playback()` before sending
  `assistant_done` and returning to listening, so the next VAD segment cannot
  transcribe the assistant's own tail. The S3 `voice_output_wait_drained()` path
  is the equivalent for a future device codec.
- **`assistant_done.text` is capped at 400 characters.** The S3 message buffer
  is 2048 bytes; the device does not need the full answer and must not abort the
  session because an optional display field is oversized.
- **THINKING and SPEAKING both drain microphone audio.** Phase 1 is explicitly
  half-duplex. Speech during thinking is lost; the user repeats after the
  assistant finishes. This is a documented phase-1 limitation, not barge-in.
- **Mac playback is test-only and serialized.** `VoiceService.play_local` raises
  its pending cap to 16 and `wait_playback()` is used per turn. `afplay` starts
  one process per sentence, so there can be gaps; the real path is the future
  board codec with the S3 `voice_output` queue. Do not call the Mac path
  "seamless device playback".
- **Conversation tools are present but execution is off.** `car_move`,
  `car_turn`, `car_stop` return `voice execution disabled` unless
  `voice_execute_commands` is explicitly enabled. Phase 1 acceptance does not
  include motion. `AgentMotionService.move` currently waits for terminal motion;
  a blocking 1 m move cannot also provide a concurrent stop, so enabling motion
  in conversation mode needs a separate non-blocking tool/stop design and is not
  part of phase 1.
- **Tool schema uses separate `car_move` and `car_turn`.** The existing
  `VoiceIntent`/`VoiceActionExecutor` has no combined distance-plus-turn
  intent; phase 1 does not silently ignore a `turn_deg`.
- **S3 clean end race.** The server sends `end` and closes; the S3 prioritizes
  `VOICE_PEER_END` over a simultaneous disconnect and treats an in-flight PCM
  send failure after `end` as success. Verified on the S3: `peer_event=0x04`,
  `listening -> cooldown -> idle`, no warning.
- **S3 profile now selects conversation mode**:
  `CONFIG_CMCAR_VOICE_MODE_CONVERSATION=y`; the command Kconfig remains
  available. `voice_transport` puts `"mode":"conversation"` in the start frame.
- **Real cloud smoke test (no motion):** one file-driven session with
  real Baidu ASR + DeepSeek + Baidu TTS returned a 3-turn
  conversation:
  1. `你好，请用一句话介绍你自己。` -> assistant reply;
  2. `那你能做什么？` -> contextual reply;
  3. `再见。` -> `好，再见`, `end(reason=user_end, turns=3)`.
  The S3 transport/wake path was separately verified through a manual trigger:
  `mode=conversation` ready, 15 s idle end, clean cooldown/idle.
- **Not verified live with the S3 microphone in this session:** Mac speaker
  injection produced no ASR segment; that is an acoustic injection limitation,
  not evidence about the microphone/ASR. The user must speak the first real
  turn.
- **Phase 2 remains barge-in**, gated on AEC/reference (ES7210 spare ADC loopback
  preferred; software reference fallback). `barge_in=false` in phase 1.
- pyserver: **206 tests passed**; new conversation host tests and the
  conversation WebSocket test. S3 build/flash passed.
