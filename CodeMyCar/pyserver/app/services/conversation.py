"""One-wake conversation with bounded TTS and device-side interruption.

The command pipeline is intentionally untouched. This module owns:

    VAD turn -> ASR -> context -> DeepSeek stream -> sentence TTS -> sink

The device gates playback echo and negotiates completion/cancellation ACKs.
This owner joins every turn task before cancellation and listening resume;
negotiated interruption pre-roll preserves the user's triggering utterance.
"""

from __future__ import annotations

import array
import asyncio
import json
import logging
import math
import sys
import time
from collections.abc import AsyncIterator
from dataclasses import dataclass, field
from typing import Any, Protocol

from app.agent.persona import SYSTEM_PROMPT
from app.agent.vehicle import DriveArguments, MoveArguments, TurnArguments, VehicleGateway
from app.services.audio_metrics import measure_pcm16
from app.services.llm import LlmClient, LlmDone, LlmTextDelta
from app.services.voice_diagnostics import InterruptionCapture
from app.services.voice_service import VoiceService, VoiceSession

logger = logging.getLogger(__name__)

END_PHRASES = {"结束", "再见", "退出", "不聊了"}
# "Stop talking", not "stop the car". Handled without the LLM and without TTS.
STOP_TALKING_PHRASES = {"停一下", "停下来", "别说了", "不要说了", "安静", "stop"}
_SENTENCE_END = "。！？；…!?;.\n"
_MIN_SENTENCE = 2
_MAX_SENTENCE = 80



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
    # Barge-in: while the assistant speaks, a sustained near-end level above
    # the AEC residual interrupts the reply. Disabled unless both the device
    # and the server opt in.
    barge_in: bool = False
    barge_in_rms: int = 700
    barge_in_ms: int = 160
    barge_in_warmup_s: float = 1.5
    barge_in_cooldown_s: float = 2.5
    # Prefer the device's double-talk detector; the energy detector is only a
    # fallback for boards without a reference channel.
    barge_in_device_events: bool = True
    playback_ack: bool = False
    barge_audio: bool = False
    barge_buffer_ms: int = 4000
    barge_tail_timeout_s: float = 5.0
    playback_timeout_s: float = 60.0
    settle_timeout_s: float = 4.0
    system_prompt: str = SYSTEM_PROMPT

    @property
    def turn_end_s(self) -> float:
        return self.turn_end_ms / 1000.0


def _pcm_rms(pcm: bytes) -> float:
    """RMS of 16 kHz / 16-bit / mono PCM. Used only for barge-in gating."""
    usable = len(pcm) // 2 * 2
    if usable == 0:
        return 0.0
    samples = array.array("h")
    samples.frombytes(pcm[:usable])
    if sys.byteorder != "little":
        samples.byteswap()
    if not samples:
        return 0.0
    return math.sqrt(sum(value * value for value in samples) / len(samples))


def normalize_text(text: str) -> str:
    return text.strip().strip("。！？，,.!?；;：: \t\r\n")


def strip_stop_prefix(text: str) -> str:
    """Keep a new request attached to a stop, without substring false stops."""
    candidate = text.casefold().removeprefix("请")
    for phrase in sorted(STOP_TALKING_PHRASES, key=len, reverse=True):
        if not candidate.startswith(phrase):
            continue
        suffix = candidate[len(phrase):]
        if not suffix:
            return ""
        cleaned = suffix.lstrip("。！？，,.!?；;：: \t\r\n")
        if cleaned != suffix or cleaned.startswith(("换", "请", "讲", "我想", "帮我", "继续", "重新", "给我")):
            # Return original casing for English names in the new question.
            return text[len(text) - len(cleaned):]
    return text


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
        end = next((i for i in range(2, len(self.messages))
                    if self.messages[i].get("role") == "user"), len(self.messages))
        del self.messages[1:end]

    def _estimate_tokens(self) -> int:
        return sum(max(1, len(str(message.get("content", ""))) // 2) for message in self.messages)


# Built from the same validated argument models as the production graph.

CAR_TOOLS: list[dict[str, Any]] = [
    {"type": "function", "function": {"name": name, "description": description,
        "parameters": schema.model_json_schema() if schema else {"type": "object", "properties": {}}}}
    for name, description, schema in [
        ("car_get_state", "Read fresh connection, body pose, capabilities; no movement", None),
        ("car_drive", "Signed cm path; body turn degrees positive right, negative left even in reverse", DriveArguments),
        ("car_move", "Straight forward/backward motion in metres", MoveArguments),
        ("car_turn", "Body turn on default short forward arc, not stationary steering", TurnArguments),
        ("car_stop", "Reliable STOP; requires confirmed stopped result", None),
    ]
]


class CarToolExecutor:
    def __init__(self, service: VoiceService) -> None:
        self.service = service
        self.gateway = VehicleGateway(service)

    async def execute(self, device_id: str, call: Any) -> dict[str, Any]:
        try:
            args = json.loads(call.arguments or "{}")
            if not isinstance(args, dict):
                return {"executed": False, "reason": "invalid tool arguments"}
            return await self.gateway.invoke(device_id, call.name, args)
        except Exception as exc:  # noqa: BLE001 - tool failures must not kill a voice turn
            logger.warning("conversation tool failed: %s", type(exc).__name__)
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
        self._interrupt_task: asyncio.Task | None = None
        self._warmup_task: asyncio.Task | None = None
        self._playback_done: asyncio.Future | None = None
        self._playback_key: tuple[int, int] | None = None
        self._stop_ack: asyncio.Future | None = None
        self._settle_started = 0.0
        self._interrupted_reply = ""
        self._turn_spoken: list[str] = []
        self._last_activity = time.monotonic()
        self._started = time.monotonic()
        self._send_seq = 0
        self._barge_ms = 0
        self._barging = False
        self._barge_log_at = 0.0
        self._speaking_since = 0.0
        self._ignore_until = 0.0
        self._post_stop = False
        self._post_silence_ms = 0
        self._barge_cooldown_until = 0.0
        self._barge_audio = bytearray()
        self._barge_started = 0.0
        self._last_interrupted_turn: int | None = None
        self._diagnostic_capture: InterruptionCapture | None = None
        self._barge_input_open = False
        self._input_changed = asyncio.Event()
        self._continuations: list[bytes] = []
        self._continuation_bytes = 0

    def start(self) -> None:
        self._worker = asyncio.create_task(self._work(), name="voice-conversation-worker")
        self._watchdog = asyncio.create_task(self._watch(), name="voice-conversation-watchdog")
        self._warmup_task = asyncio.create_task(self._warmup(), name="voice-auth-warmup")

    async def _warmup(self) -> None:
        async def prepare(provider) -> None:
            try:
                await asyncio.wait_for(provider.warmup(), timeout=5)
            except Exception:  # noqa: BLE001 - optional preparation never kills a conversation
                logger.warning("voice auth warmup unavailable; request will retry")
        async with asyncio.TaskGroup() as group:
            for provider in (self.service.asr, self.service.tts):
                if callable(getattr(provider, "warmup", None)):
                    group.create_task(prepare(provider))

    def feed(self, pcm: bytes) -> None:
        if self.ended.is_set():
            return
        if self._diagnostic_capture is not None:
            self._diagnostic_capture.feed(pcm, self.state)
        self._consume_pcm(pcm)

    def _consume_pcm(self, pcm: bytes) -> None:
        if self._barging and self.config.barge_audio:
            # The receive owner keeps consuming while cancellation waits for
            # stop_ack. Bound this temporary audio and account it only once,
            # when replaying through VAD after the old worker has joined.
            if len(self._barge_audio) + len(pcm) > self.config.barge_buffer_ms * 32:
                self.request_end("barge_audio_overflow")
                return
            self._barge_audio.extend(pcm)
            return
        if self._post_stop:
            # The words that triggered the stop are not a new question. Drop
            # the rest of that utterance and only start listening again after
            # the user goes quiet, so "停一下" cannot become an LLM turn.
            if measure_pcm16(pcm[:len(pcm) // 2 * 2]).ac_rms < 250:
                self._post_silence_ms += max(1, len(pcm) // 32)
            else:
                self._post_silence_ms = 0
            self.session.feed(pcm, detect=False)
            self._last_activity = time.monotonic()
            if self._post_silence_ms >= 800:
                self._post_stop = False
                self.session.discard()
                self.state = "listening"
                self._last_activity = time.monotonic()
                logger.info("barge-in settled; listening for a new question")
            elif time.monotonic() - self._settle_started > self.config.settle_timeout_s:
                self.request_end("settle_timeout")
            return
        if self.state != "listening":
            if self.state == "thinking" and self._barge_input_open:
                # A comma/pause can close the stop phrase before cloud ASR
                # returns. Retain the rest of THIS interrupting user turn;
                # do not queue a new turn or advance its response ID yet.
                for segment in self.session.feed(pcm):
                    if not self._retain_continuation(segment.pcm):
                        return
                self._input_changed.set()
                return
            # Never transcribe the assistant's own audio; with barge-in the
            # raw level still decides whether the user is interrupting.
            self.session.feed(pcm, detect=False)
            self._maybe_barge_in(pcm)
            return
        if time.monotonic() < self._ignore_until:
            # Speaker tail right after the barge-in stop; do not let it join
            # the user's first words.
            self.session.feed(pcm, detect=False)
            return
        for segment in self.session.feed(pcm):
            if self.state == "thinking" and self._barge_input_open:
                if not self._retain_continuation(segment.pcm):
                    return
                continue
            if self.state != "listening":
                break
            if self.turn >= self.config.max_turns:
                self.request_end("max_turns")
                return
            if self._queue.full():
                self.request_end("backlog")
                return
            self.turn += 1
            self.state = "thinking"
            self._queue.put_nowait(segment)

    def _retain_continuation(self, pcm: bytes) -> bool:
        if self._continuation_bytes + len(pcm) > self.service.config.max_segment_s * 32000:
            self.request_end("barge_input_overflow")
            return False
        self._continuation_bytes += len(pcm)
        self._continuations.append(pcm)
        self._input_changed.set()
        return True

    def _maybe_barge_in(self, pcm: bytes) -> None:
        if self.config.barge_in_device_events:
            return
        if not self.config.barge_in or self.state != "speaking":
            self._barge_ms = 0
            return
        # AEC needs a moment to converge at the start of a reply; residual
        # echo spikes there look exactly like near-end speech.
        if time.monotonic() - self._speaking_since < self.config.barge_in_warmup_s:
            self._barge_ms = 0
            return
        if self.ended.is_set() or self._barging:
            return
        # 16 kHz mono -> 32 bytes per millisecond.
        rms = _pcm_rms(pcm)
        now = time.monotonic()
        if now - self._barge_log_at >= 0.5:
            self._barge_log_at = now
            logger.info("barge level rms=%.0f threshold=%d held_ms=%d",
                        rms, self.config.barge_in_rms, self._barge_ms)
        # Speech between Chinese syllables dips below any single threshold;
        # accumulate above-threshold frames and decay on quiet frames instead
        # of resetting, otherwise a real interruption never reaches 180 ms.
        if rms >= self.config.barge_in_rms:
            self._barge_ms += max(1, len(pcm) // 32)
        else:
            self._barge_ms = max(0, self._barge_ms - 25)
        if self._barge_ms >= self.config.barge_in_ms:
            self.device_barge_in()

    def device_barge_in(self, turn: int | None = None) -> None:
        """Called when the device itself detected near-end speech."""
        if (self.ended.is_set() or not self.config.barge_in or self._barging
                or self.state != "speaking"
                or (turn is not None and turn != self.turn)
                or self.turn == self._last_interrupted_turn
                or (turn is None and time.monotonic() < self._barge_cooldown_until)):
            return
        # A valid event for a NEW reply means the device already stopped it.
        # Ignoring that event on a server-only cooldown would strand the peer
        # waiting for STOP while we keep sending cancelled audio.
        self._last_interrupted_turn = self.turn
        self._barging = True
        self._barge_audio.clear()
        self._barge_started = time.monotonic()
        self._barge_input_open = self.config.barge_audio
        self._continuations.clear()
        self._continuation_bytes = 0
        if self.service.config.archive_continuous_segments and self.service.reserve_archive():
            if self._diagnostic_capture is not None:
                self._diagnostic_capture.close()
            path = self.session.artifact_dir / f"{self.session.stem}-barge{self.turn:02d}-uplink.wav"
            self._diagnostic_capture = InterruptionCapture(path)
        self._interrupt_task = asyncio.create_task(
            self._begin_barge_in(), name="voice-conversation-interrupt")

    def playback_event(self, event: dict) -> None:
        """Only the current response's matching device completion can release a waiter."""
        if event.get("type") == "stop_ack":
            if (event.get("turn") == self.turn and self._stop_ack is not None
                    and not self._stop_ack.done()):
                self._stop_ack.set_result(None)
                logger.info("voice_metric %s", json.dumps({
                    "name": "barge_stop_ack", "turn": self.turn,
                    "server_ack_ms": round((time.monotonic() - self._barge_started) * 1000, 1),
                    "local_dma_drain_ms": event.get("local_dma_drain_ms"),
                }))
            return
        key = (event.get("turn"), event.get("seq"))
        if (key == self._playback_key and self._playback_done is not None
                and not self._playback_done.done()):
            self._playback_done.set_result(None)

    async def _wait_playback(self) -> None:
        if not self.config.playback_ack or self._playback_key is None:
            await self.service.wait_playback()
            return
        await self._safe_send({"type": "playback_end", "turn": self._playback_key[0],
                               "seq": self._playback_key[1]})
        try:
            await asyncio.wait_for(asyncio.shield(self._playback_done),
                                   timeout=self.config.playback_timeout_s)
        except TimeoutError:
            self.request_end("playback_timeout")
            raise

    async def _begin_barge_in(self) -> None:
        """Stop the reply now, drop queued turns, and listen to the user."""
        if self.ended.is_set():
            return
        interrupted_turn = self.turn
        self.state = "interrupting"
        logger.info("barge-in: interrupting assistant")
        # Cancel first: the worker may be inside a paced audio send holding
        # the sink lock, and `stop` must not queue behind the rest of a
        # sentence. Cancellation unwinds that send and releases the lock.
        worker, self._worker = self._worker, None
        if worker is not None and worker is not asyncio.current_task():
            worker.cancel()
            await asyncio.gather(worker, return_exceptions=True)
        self._interrupted_reply = "".join(self._turn_spoken) or "（回复尚未完整下发）"
        try:
            if self.config.playback_ack:
                self._stop_ack = asyncio.get_running_loop().create_future()
            await asyncio.wait_for(self._safe_send({"type": "stop", "turn": interrupted_turn}), 2)
            if self._stop_ack is not None:
                await asyncio.wait_for(self._stop_ack, 2)
        except TimeoutError:
            self.request_end("stop_timeout")
            return
        if self.ended.is_set():
            return
        while True:
            try:
                self._queue.get_nowait()
            except asyncio.QueueEmpty:
                break
            else:
                self._queue.task_done()
        self.session.discard()
        self.state = "listening"
        self._barge_cooldown_until = time.monotonic() + self.config.barge_in_cooldown_s
        self._post_stop = not self.config.barge_audio
        self._settle_started = time.monotonic()
        self._post_silence_ms = 0
        self._ignore_until = 0 if self.config.barge_audio else time.monotonic() + 0.35
        self._last_activity = time.monotonic()
        self._barge_ms = 0
        self._barge_log_at = 0.0
        self._speaking_since = 0.0
        self._playback_key = None
        self._playback_done = None
        self._stop_ack = None
        self._barging = False
        if not self.ended.is_set() and self._worker is None:
            self._worker = asyncio.create_task(
                self._work(), name="voice-conversation-worker"
            )
        if self.config.barge_audio and not self.ended.is_set():
            pending = bytes(self._barge_audio)
            self._barge_audio.clear()
            self._consume_pcm(pending)

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
        if self._interrupt_task is not None and self._interrupt_task is not asyncio.current_task():
            self._interrupt_task.cancel()
        if self._warmup_task is not None and self._warmup_task is not asyncio.current_task():
            self._warmup_task.cancel()

    async def close(self, abort: bool = False) -> None:
        self.request_end("abort" if abort else (self.end_reason or "close"))
        for task in (self._interrupt_task, self._worker, self._watchdog, self._warmup_task):
            if task is not None and task is not asyncio.current_task():
                task.cancel()
                await asyncio.gather(task, return_exceptions=True)
        self._worker = self._watchdog = self._interrupt_task = self._warmup_task = None
        self._barge_audio.clear()
        if self._diagnostic_capture is not None:
            self._diagnostic_capture.close()
            self._diagnostic_capture = None
        self._continuations.clear()
        self._barge_input_open = False

    async def _watch(self) -> None:
        try:
            while not self.ended.is_set():
                await asyncio.sleep(0.5)
                now = time.monotonic()
                if (self._diagnostic_capture is not None and
                        now - self._diagnostic_capture.started >= self._diagnostic_capture.seconds):
                    self._diagnostic_capture.close()
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
                self._turn_spoken = []
                self._playback_key = None
                self._playback_done = None
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

    async def _recognize_turn(self, segment) -> str | None:
        pcm = segment.pcm
        try:
            # Bounded extensions, no detached ASR tasks and no concurrent LLM
            # admission while the user is still finishing the interruption.
            for _ in range(3):
                started = time.monotonic()
                text = await self.service.asr.recognize(pcm)
                logger.info("voice timing turn=%d asr_ms=%.0f", self.turn,
                            (time.monotonic() - started) * 1000)
                if not self._barge_input_open:
                    return text
                deadline = time.monotonic() + self.config.barge_tail_timeout_s
                while self.session.in_speech and not self.ended.is_set():
                    self._input_changed.clear()
                    remaining = deadline - time.monotonic()
                    if remaining <= 0:
                        self.request_end("barge_input_timeout")
                        return None
                    try:
                        await asyncio.wait_for(self._input_changed.wait(),
                                               remaining)
                    except TimeoutError:
                        self.request_end("barge_input_timeout")
                        return None
                if self.ended.is_set():
                    return None
                if not self._continuations:
                    return text
                extension = b"".join(self._continuations)
                self._continuations.clear()
                self._continuation_bytes = 0
                # A recognized stand-alone stop was already executed locally.
                # Re-recognize the following request alone, avoiding a long
                # internal silence or replaying that stop to the LLM.
                if not strip_stop_prefix(normalize_text(text or "")):
                    pcm = extension
                else:
                    pcm += extension
                if len(pcm) > self.service.config.max_segment_s * 32000:
                    self.request_end("barge_input_overflow")
                    return None
                logger.info("barge input continued during ASR; retaining %d bytes", len(extension))
            self.request_end("barge_input_extensions")
            return None
        finally:
            self._barge_input_open = False
            self._continuations.clear()
            self._continuation_bytes = 0

    async def _handle_turn(self, segment) -> None:
        text = None
        if self.service.asr is not None:
            try:
                text = await self._recognize_turn(segment)
            except Exception as exc:  # noqa: BLE001
                logger.warning("conversation ASR failed: %s", exc)
        if not text:
            await self._safe_send({"type": "error", "message": "没有听清，请再说一次"})
            return

        if self.service.config.archive_continuous_segments and segment.path.exists():
            logger.info("voice diagnostic transcript turn=%d text=%r", self.turn, text)

        await self._safe_send({"type": "user_text", "turn": self.turn, "text": text[:400], "intent": None})
        normalized = strip_stop_prefix(normalize_text(text))
        if not normalized:
            await self._safe_send(
                {"type": "assistant_done", "turn": self.turn, "text": "",
                 "interrupted": True}
            )
            self.state = "listening"
            self._last_activity = time.monotonic()
            return
        if normalized in END_PHRASES:
            await self._speak_text("好，再见")
            if self.service.tts is not None:
                try:
                    await asyncio.wait_for(self._wait_playback(), timeout=self.config.playback_timeout_s)
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

        self.history.add_user(normalized)
        final_text = await self._answer()
        if final_text is not None:
            # A v2 device confirms its DMA tail drained before listening resumes.
            if self.service.tts is not None:
                try:
                    await self._wait_playback()
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
        if self._interrupted_reply:
            # Do not claim that generated or transmitted audio was heard. The
            # adapter receives an explicit interruption note on the next turn.
            text = ("[上一条回复被用户打断；以下是已下发但不保证已听完的片段："
                    + self._interrupted_reply[:600] + "。若用户要求继续，请从合适边界重述。]\n"
                    + text)
            self._interrupted_reply = ""
        sentences: asyncio.Queue[str | None] = asyncio.Queue(maxsize=2)
        prepared: asyncio.Queue = asyncio.Queue(maxsize=2)

        async def generate() -> None:
            async for delta in self.agent_session.stream(text):
                for sentence in chunker.feed(delta):
                    await sentences.put(sentence)
            for sentence in chunker.flush():
                await sentences.put(sentence)
            await sentences.put(None)

        async def synthesize() -> None:
            while (sentence := await sentences.get()) is not None:
                audio = await self._synthesize(sentence)
                await prepared.put((sentence, audio))
            await prepared.put(None)

        # Bounded prefetch overlaps model, cloud synthesis and paced playback.
        # Cancellation of this turn joins BOTH children before stop is sent.
        async with asyncio.TaskGroup() as group:
            group.create_task(generate(), name="voice-model-producer")
            group.create_task(synthesize(), name="voice-tts-producer")
            while (item := await prepared.get()) is not None:
                if self.ended.is_set():
                    raise asyncio.CancelledError
                if self.state != "speaking":
                    self._speaking_since = time.monotonic()
                self.state = "speaking"
                await self._speak_sentence(item[0], spoken, audio=item[1], prepared=True)
        if self.service.tts is not None:
            try:
                await self._wait_playback()
            except TimeoutError:
                logger.warning("conversation playback did not drain")
        final_text = "".join(spoken)
        if final_text and not self.ended.is_set():
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
                tools=([t for t in CAR_TOOLS if self.service.config.execute_commands
                        or t["function"]["name"] in {"car_get_state", "car_stop"}]),
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

    async def _synthesize(self, sentence: str):
        if self.service.tts is None:
            return None
        try:
            started = time.monotonic()
            audio = await self.service.tts.synthesize(sentence, aue=4)
            logger.info("voice timing turn=%d tts_ms=%.0f audio_ms=%.0f", self.turn,
                        (time.monotonic() - started) * 1000,
                        len(audio.data) / (audio.sample_rate * audio.channels * 2) * 1000)
            return audio
        except Exception as exc:  # noqa: BLE001 - injected synthesis backends may vary
            logger.warning("conversation TTS failed: %s", exc)
            await self._safe_send({"type": "error", "message": "语音合成失败"})
            return None

    async def _speak_sentence(self, sentence: str, spoken: list[str], *,
                              audio=None, prepared: bool = False) -> None:
        sentence = sentence.strip()
        if not sentence:
            return
        await self._safe_send(
            {"type": "assistant_delta", "turn": self.turn, "text": sentence}
        )
        if self.service.tts is None:
            spoken.append(sentence)
            return
        if not prepared:
            audio = await self._synthesize(sentence)
        if audio is None:
            return
        self._send_seq += 1
        self._expect_playback()
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
            self.request_end("playback_send_failed")
            return
        spoken.append(sentence)
        self._turn_spoken.append(sentence)
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
        self._expect_playback()
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

    def _expect_playback(self) -> None:
        self._playback_key = (self.turn, self._send_seq)
        self._playback_done = asyncio.get_running_loop().create_future()

    async def _safe_send(self, payload: dict[str, Any]) -> None:
        try:
            await self.sink.send_json(payload)
        except Exception:
            logger.debug("conversation send failed", exc_info=True)
