"""One-wake, multi-turn voice conversation (phase 1: half-duplex).

The command pipeline is intentionally untouched. This module owns:

    VAD turn -> ASR -> context -> DeepSeek stream -> sentence TTS -> sink

It drains microphone audio while a turn is thinking/speaking, so the assistant
never transcribes its own TTS. Barge-in is phase 2.
"""

from __future__ import annotations

import asyncio
import json
import logging
import time
from collections.abc import AsyncIterator
from dataclasses import dataclass, field
from typing import Any, Protocol

from app.services.llm import LlmClient, LlmDone, LlmTextDelta
from app.services.voice_intent import VoiceIntent
from app.services.voice_service import VoiceService, VoiceSession

logger = logging.getLogger(__name__)

END_PHRASES = {"结束", "再见", "退出", "不聊了"}
_SENTENCE_END = "。！？；…!?;.\n"
_MIN_SENTENCE = 2
_MAX_SENTENCE = 80

SYSTEM_PROMPT = """你是 CodeMyCar 车载语音助手，正在和车主进行免提对话。
用简洁、自然的口语回答，通常 1 到 3 句话；不要输出 Markdown、代码块或列表。
默认用中文回答，用户说英文时可以用英文。
车辆动作必须通过工具完成，不要声称已经移动，除非工具结果明确表示执行成功。
如果语音运动执行被关闭，直接说明当前不能控制车辆，不要假装动作完成。
危险动作要先询问确认；不要提供危险驾驶建议。
用户说“结束”“再见”“退出”“不聊了”表示结束本次对话。"""


class ConversationSink(Protocol):
    async def send_json(self, payload: dict[str, Any]) -> None: ...
    async def send_bytes(self, payload: bytes) -> None: ...
    async def send_audio(self, header: dict[str, Any], payload: bytes) -> None: ...


@dataclass(frozen=True)
class ConversationConfig:
    turn_end_ms: int = 700
    idle_s: float = 15.0
    max_s: float = 600.0
    max_turns: int = 30
    history_turns: int = 12
    history_tokens: int = 8000
    max_tool_rounds: int = 3
    max_tokens: int = 512
    system_prompt: str = SYSTEM_PROMPT

    @property
    def turn_end_s(self) -> float:
        return self.turn_end_ms / 1000.0


def normalize_text(text: str) -> str:
    return text.strip().strip("。！？，,.!?；;：: \t\r\n")


class SentenceChunker:
    """Incremental Chinese/English sentence splitter for streaming TTS."""

    def __init__(self, min_len: int = _MIN_SENTENCE, max_len: int = _MAX_SENTENCE) -> None:
        self.min_len = min_len
        self.max_len = max_len
        self.buffer = ""

    def feed(self, text: str) -> list[str]:
        self.buffer += text
        out: list[str] = []
        while True:
            cut = -1
            for index, char in enumerate(self.buffer):
                if char in _SENTENCE_END:
                    cut = index + 1
                    break
            if cut < 0:
                # A long clause without punctuation still needs to start audio.
                if len(self.buffer) >= self.max_len:
                    cut = self._soft_cut(self.max_len)
                else:
                    break
            piece = self.buffer[:cut].strip()
            self.buffer = self.buffer[cut:]
            if len(piece) >= self.min_len:
                out.append(piece)
            elif piece:
                self.buffer = piece + self.buffer
                break
        return out

    def _soft_cut(self, limit: int) -> int:
        for separator in ("，", ",", "、", " "):
            index = self.buffer.rfind(separator, 0, limit)
            if index > self.min_len:
                return index + 1
        return limit

    def flush(self) -> list[str]:
        piece = self.buffer.strip()
        self.buffer = ""
        return [piece] if piece else []


@dataclass
class ConversationHistory:
    system_prompt: str
    max_turns: int = 12
    max_tokens: int = 8000
    messages: list[dict[str, Any]] = field(default_factory=list)

    def __post_init__(self) -> None:
        self.messages = [{"role": "system", "content": self.system_prompt}]

    def add_user(self, text: str) -> None:
        self.messages.append({"role": "user", "content": text})
        self._trim()

    def add_assistant(self, text: str, tool_calls: list[dict] | None = None) -> None:
        message: dict[str, Any] = {"role": "assistant", "content": text}
        if tool_calls:
            message["tool_calls"] = tool_calls
        self.messages.append(message)
        self._trim()

    def add_tool(self, tool_call_id: str, content: str) -> None:
        self.messages.append(
            {"role": "tool", "tool_call_id": tool_call_id, "content": content}
        )
        self._trim()

    def request_messages(self) -> list[dict[str, Any]]:
        self._trim()
        return list(self.messages)

    def _trim(self) -> None:
        # Preserve the system prompt. Drop oldest complete user turns first.
        while len(self.messages) > 1 + self.max_turns * 2:
            self._drop_oldest_turn()
        while self._estimate_tokens() > self.max_tokens and len(self.messages) > 2:
            self._drop_oldest_turn()

    def _drop_oldest_turn(self) -> None:
        if len(self.messages) <= 1:
            return
        # Remove the oldest user and everything up to the next user message,
        # keeping the system prompt at index 0.
        del self.messages[1]
        while len(self.messages) > 1 and self.messages[1].get("role") == "tool":
            del self.messages[1]

    def _estimate_tokens(self) -> int:
        return sum(max(1, len(str(message.get("content", ""))) // 2) for message in self.messages)


CAR_TOOLS: list[dict[str, Any]] = [
    {
        "type": "function",
        "function": {
            "name": "car_move",
            "description": "Move the car forward or backward by a short distance. "
            "Motion execution may be disabled by the server.",
            "parameters": {
                "type": "object",
                "properties": {
                    "direction": {"type": "string", "enum": ["forward", "backward"]},
                    "distance_m": {"type": "number", "minimum": 0.05, "maximum": 1.0},
                },
                "required": ["direction", "distance_m"],
            },
        },
    },
    {
        "type": "function",
        "function": {
            "name": "car_turn",
            "description": "Turn the car left or right by an angle. "
            "Motion execution may be disabled by the server.",
            "parameters": {
                "type": "object",
                "properties": {
                    "direction": {"type": "string", "enum": ["left", "right"]},
                    "angle_deg": {"type": "number", "minimum": 1, "maximum": 90},
                },
                "required": ["direction", "angle_deg"],
            },
        },
    },
    {
        "type": "function",
        "function": {
            "name": "car_stop",
            "description": "Immediately request the car to stop.",
            "parameters": {"type": "object", "properties": {}},
        },
    },
]


class CarToolExecutor:
    def __init__(self, service: VoiceService) -> None:
        self.service = service

    async def execute(self, device_id: str, call: Any) -> dict[str, Any]:
        if not self.service.config.execute_commands:
            return {"executed": False, "reason": "voice execution disabled"}
        try:
            args = json.loads(call.arguments or "{}")
        except json.JSONDecodeError:
            return {"executed": False, "reason": "invalid tool arguments"}
        try:
            if call.name == "car_move":
                direction = str(args.get("direction", "")).lower()
                distance_m = float(args.get("distance_m", 0))
                if direction not in ("forward", "backward"):
                    return {"executed": False, "reason": "invalid direction"}
                intent = VoiceIntent(
                    direction,
                    distance_mm=round(distance_m * 1000),
                )
                return await self.service.execute_intent(device_id, intent)
            if call.name == "car_turn":
                direction = str(args.get("direction", "")).lower()
                angle = round(float(args.get("angle_deg", 0)))
                if direction not in ("left", "right") or angle <= 0:
                    return {"executed": False, "reason": "invalid turn"}
                return await self.service.execute_intent(
                    device_id,
                    VoiceIntent(
                        "turn_left" if direction == "left" else "turn_right",
                        angle_deg=angle,
                    ),
                )
            if call.name == "car_stop":
                await self.service.stop_action(device_id)
                return {"executed": True, "action": "stop"}
            return {"executed": False, "reason": f"unknown tool {call.name}"}
        except Exception as exc:  # noqa: BLE001 - tool errors must not kill the session
            logger.warning("conversation tool %s failed: %s", call.name, exc)
            return {"executed": False, "reason": "tool failed"}


class ConversationAgentSession(Protocol):
    def stream(self, text: str) -> AsyncIterator[str]: ...


class ConversationRunner:
    """Owns one conversation session; the route only feeds frames and ends it."""

    def __init__(
        self,
        service: VoiceService,
        session: VoiceSession,
        sink: ConversationSink,
        *,
        llm: LlmClient,
        config: ConversationConfig | None = None,
        device_id: str | None = None,
        agent_session: ConversationAgentSession | None = None,
    ) -> None:
        self.service = service
        self.session = session
        self.sink = sink
        self.llm = llm
        self.agent_session = agent_session
        self.config = config or ConversationConfig()
        self.device_id = device_id or session.device_id
        self.history = ConversationHistory(
            self.config.system_prompt,
            max_turns=self.config.history_turns,
            max_tokens=self.config.history_tokens,
        )
        self.tools = CarToolExecutor(service)
        self.state = "listening"
        self.turn = 0
        self.end_reason: str | None = None
        self.ended = asyncio.Event()
        self._queue: asyncio.Queue = asyncio.Queue(maxsize=2)
        self._worker: asyncio.Task | None = None
        self._watchdog: asyncio.Task | None = None
        self._last_activity = time.monotonic()
        self._started = time.monotonic()
        self._send_seq = 0

    def start(self) -> None:
        self._worker = asyncio.create_task(self._work(), name="voice-conversation-worker")
        self._watchdog = asyncio.create_task(self._watch(), name="voice-conversation-watchdog")

    def feed(self, pcm: bytes) -> None:
        if self.ended.is_set():
            return
        if self.state != "listening":
            # Half-duplex: drain user audio while thinking/speaking.
            self.session.feed(pcm, detect=False)
            return
        for segment in self.session.feed(pcm):
            if self.turn >= self.config.max_turns:
                self.request_end("max_turns")
                return
            if self._queue.full():
                self.request_end("backlog")
                return
            self.turn += 1
            self.state = "thinking"
            self._queue.put_nowait(segment)

    async def finish(self) -> None:
        for segment in self.session.finish():
            if self._queue.full():
                break
            self.turn += 1
            self.state = "thinking"
            self._queue.put_nowait(segment)
        await self._queue.join()
        if self._worker is not None:
            await self._worker

    def request_end(self, reason: str) -> None:
        if self.ended.is_set():
            return
        self.end_reason = reason
        self.state = "ending"
        self.ended.set()
        if self._worker is not None and self._worker is not asyncio.current_task():
            self._worker.cancel()
        if self._watchdog is not None and self._watchdog is not asyncio.current_task():
            self._watchdog.cancel()

    async def close(self, abort: bool = False) -> None:
        self.request_end("abort" if abort else (self.end_reason or "close"))
        for task in (self._worker, self._watchdog):
            if task is not None and task is not asyncio.current_task():
                task.cancel()
                await asyncio.gather(task, return_exceptions=True)
        self._worker = self._watchdog = None

    async def _watch(self) -> None:
        try:
            while not self.ended.is_set():
                await asyncio.sleep(0.5)
                now = time.monotonic()
                if now - self._started > self.config.max_s:
                    self.request_end("max_duration")
                    return
                if (
                    self.state == "listening"
                    and now - self._last_activity >= self.config.idle_s
                ):
                    self.request_end("idle")
                    return
        except asyncio.CancelledError:
            return

    async def _work(self) -> None:
        try:
            while not self.ended.is_set():
                segment = await self._queue.get()
                try:
                    await self._handle_turn(segment)
                except asyncio.CancelledError:
                    raise
                except Exception:
                    logger.exception("conversation turn failed")
                    await self._safe_send({"type": "error", "message": "conversation turn failed"})
                finally:
                    self._queue.task_done()
                if not self.ended.is_set():
                    self.state = "listening"
                    self._last_activity = time.monotonic()
        except asyncio.CancelledError:
            return

    async def _handle_turn(self, segment) -> None:
        text = None
        if self.service.asr is not None:
            try:
                text = await self.service.asr.recognize(segment.pcm)
            except Exception as exc:  # noqa: BLE001
                logger.warning("conversation ASR failed: %s", exc)
        if not text:
            await self._safe_send({"type": "error", "message": "没有听清，请再说一次"})
            return

        await self._safe_send({"type": "user_text", "turn": self.turn, "text": text[:400], "intent": None})
        normalized = normalize_text(text)
        if normalized in END_PHRASES:
            await self._speak_text("好，再见")
            if self.service.tts is not None:
                try:
                    await asyncio.wait_for(self.service.wait_playback(), timeout=10.0)
                except TimeoutError:
                    logger.warning("conversation closing playback did not drain")
            await self._safe_send(
                {"type": "assistant_done", "turn": self.turn, "text": "好，再见", "interrupted": False}
            )
            self.request_end("user_end")
            return

        if self.agent_session is not None:
            await self._answer_with_agent(normalized)
            return

        self.history.add_user(text)
        final_text = await self._answer()
        if final_text is not None:
            # Phase 1 uses the Mac player; do not return to LISTENING while the
            # assistant's tail is still audible or the next VAD segment can
            # transcribe it as a user turn.
            if self.service.tts is not None:
                try:
                    await asyncio.wait_for(self.service.wait_playback(), timeout=60.0)
                except TimeoutError:
                    logger.warning("conversation playback did not drain")
            await self._safe_send(
                {
                    "type": "assistant_done",
                    "turn": self.turn,
                    "text": final_text[:400],
                    "interrupted": False,
                }
            )

    async def _answer_with_agent(self, text: str) -> None:
        """Deep Agents owns history/tools/summarization; this layer owns TTS."""
        if self.agent_session is None:
            return
        spoken: list[str] = []
        chunker = SentenceChunker()
        async for delta in self.agent_session.stream(text):
            for sentence in chunker.feed(delta):
                self.state = "speaking"
                await self._speak_sentence(sentence, spoken)
        for sentence in chunker.flush():
            self.state = "speaking"
            await self._speak_sentence(sentence, spoken)
        if self.service.tts is not None:
            try:
                await asyncio.wait_for(self.service.wait_playback(), timeout=60.0)
            except TimeoutError:
                logger.warning("conversation playback did not drain")
        final_text = "".join(spoken)
        if final_text:
            await self._safe_send(
                {
                    "type": "assistant_done",
                    "turn": self.turn,
                    "text": final_text[:400],
                    "interrupted": False,
                }
            )

    async def _answer(self) -> str | None:
        spoken: list[str] = []
        final_text = ""
        for round_index in range(self.config.max_tool_rounds + 1):
            done: LlmDone | None = None
            chunker = SentenceChunker()
            async for event in self.llm.stream_chat(
                self.history.request_messages(),
                tools=CAR_TOOLS,
            ):
                if isinstance(event, LlmTextDelta):
                    for sentence in chunker.feed(event.text):
                        self.state = "speaking"
                        await self._speak_sentence(sentence, spoken)
                elif isinstance(event, LlmDone):
                    done = event
            if done is None:
                break
            for sentence in chunker.flush():
                self.state = "speaking"
                await self._speak_sentence(sentence, spoken)

            if not done.tool_calls or round_index >= self.config.max_tool_rounds:
                final_text = done.content.strip()
                if final_text and not spoken:
                    # Content existed but the chunker had no punctuation.
                    self.state = "speaking"
                    await self._speak_sentence(final_text, spoken)
                self.history.add_assistant(final_text)
                return final_text or "".join(spoken)

            tool_calls_payload = [
                {
                    "id": call.id,
                    "type": "function",
                    "function": {"name": call.name, "arguments": call.arguments},
                }
                for call in done.tool_calls
            ]
            self.history.add_assistant(done.content, tool_calls_payload)
            for call in done.tool_calls:
                result = await self.tools.execute(self.device_id, call)
                self.history.add_tool(call.id, json.dumps(result, ensure_ascii=False))
        # If the loop ended without a final answer, still return spoken text.
        final_text = "".join(spoken)
        if final_text:
            self.history.add_assistant(final_text)
        return final_text or None

    async def _speak_sentence(self, sentence: str, spoken: list[str]) -> None:
        sentence = sentence.strip()
        if not sentence:
            return
        spoken.append(sentence)
        await self._safe_send(
            {"type": "assistant_delta", "turn": self.turn, "text": sentence}
        )
        if self.service.tts is None:
            return
        try:
            audio = await self.service.tts.synthesize(sentence, aue=4)
        except Exception as exc:  # noqa: BLE001
            logger.warning("conversation TTS failed: %s", exc)
            await self._safe_send({"type": "error", "message": "语音合成失败"})
            return
        self._send_seq += 1
        header = {
            "type": "speak",
            "turn": self.turn,
            "seq": self._send_seq,
            "text": sentence,
            "format": audio.format,
            "rate": audio.sample_rate,
            "channels": audio.channels,
            "bytes": len(audio.data),
        }
        try:
            await self.sink.send_audio(header, audio.data)
        except Exception:
            logger.debug("conversation speak send failed", exc_info=True)
        self.service.play_local(audio)

    async def _speak_text(self, text: str) -> None:
        if self.service.tts is None:
            return
        try:
            audio = await self.service.tts.synthesize(text, aue=4)
        except Exception as exc:  # noqa: BLE001 - closing a session must not fail
            logger.warning("conversation closing TTS failed: %s", exc)
            return
        self._send_seq += 1
        header = {
            "type": "speak",
            "turn": self.turn,
            "seq": self._send_seq,
            "text": text,
            "format": audio.format,
            "rate": audio.sample_rate,
            "channels": audio.channels,
            "bytes": len(audio.data),
        }
        try:
            await self.sink.send_audio(header, audio.data)
        except Exception:
            logger.debug("conversation closing speak send failed", exc_info=True)
        self.service.play_local(audio)

    async def _safe_send(self, payload: dict[str, Any]) -> None:
        try:
            await self.sink.send_json(payload)
        except Exception:
            logger.debug("conversation send failed", exc_info=True)
