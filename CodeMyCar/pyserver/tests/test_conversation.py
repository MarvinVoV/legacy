"""Conversation mode tests with fake ASR/TTS/LLM; no network or audio device."""

import asyncio

import pytest

from app.services.conversation import (
    ConversationConfig,
    ConversationHistory,
    ConversationRunner,
    SentenceChunker,
)
from app.services.llm import LlmDone, LlmTextDelta
from tests.test_voice_ws import FakeAsr, FakeLocalPlayer, FakeTts, make_service, pcm


class FakeLlm:
    def __init__(self, answers: list[str]) -> None:
        self.answers = list(answers)
        self.calls = []

    async def stream_chat(self, messages, *, tools=None, cancel=None):
        self.calls.append(messages)
        text = self.answers.pop(0)
        cut = max(1, len(text) // 2)
        yield LlmTextDelta(text[:cut])
        yield LlmTextDelta(text[cut:])
        yield LlmDone(content=text, tool_calls=())


class FakeSink:
    def __init__(self) -> None:
        self.events: list[tuple] = []
        self.done = asyncio.Event()

    async def send_json(self, payload):
        self.events.append(("json", payload))
        if payload.get("type") in ("assistant_done", "end"):
            self.done.set()

    async def send_bytes(self, payload):
        self.events.append(("bytes", payload))

    async def send_audio(self, header, payload):
        self.events.append(("audio", header, payload))


def json_events(sink: FakeSink) -> list[dict]:
    return [event[1] for event in sink.events if event[0] == "json"]


async def make_runner(tmp_path, *, answer: str = "你好，我在。", asr_text: str = "你好"):
    asr = FakeAsr(asr_text)
    tts = FakeTts()
    player = FakeLocalPlayer()
    service = make_service(tmp_path, asr, tts=tts, local_player=player)
    session = service.create_session("DEV_1", continuous=True, end_silence_ms=200)
    sink = FakeSink()
    llm = FakeLlm([answer])
    runner = ConversationRunner(
        service,
        session,
        sink,
        llm=llm,
        config=ConversationConfig(idle_s=5, turn_end_ms=200),
    )
    runner.start()
    return service, session, runner, sink, llm, tts, player


async def close_runner(service, session, runner):
    await runner.close(abort=True)
    session.close()
    service.release(session)
    await service.aclose()


def test_sentence_chunker_splits_and_flushes():
    chunker = SentenceChunker()
    assert chunker.feed("你好。今天天气") == ["你好。"]
    assert chunker.feed("不错！") == ["今天天气不错！"]
    chunker.feed("没有标点")
    assert chunker.flush() == ["没有标点"]


def test_history_keeps_system_and_trims():
    history = ConversationHistory("system", max_turns=2, max_tokens=1000)
    history.add_user("u1")
    history.add_assistant("a1")
    history.add_user("u2")
    history.add_assistant("a2")
    history.add_user("u3")
    history.add_assistant("a3")
    messages = history.request_messages()
    assert messages[0] == {"role": "system", "content": "system"}
    assert len(messages) <= 1 + 2 * 2


@pytest.mark.asyncio
async def test_one_turn_streams_speech_and_assistant_done(tmp_path):
    service, session, runner, sink, _llm, tts, player = await make_runner(tmp_path)
    runner.feed(pcm(0.3, 3000) + pcm(0.3))
    await asyncio.wait_for(sink.done.wait(), timeout=2)
    events = json_events(sink)
    assert any(e["type"] == "user_text" and e["text"] == "你好" for e in events)
    done = [e for e in events if e["type"] == "assistant_done"]
    assert done and done[0]["text"] == "你好，我在。"
    audio = [e for e in sink.events if e[0] == "audio"]
    assert audio and audio[0][1]["type"] == "speak"
    assert audio[0][1]["turn"] == 1
    assert tts.texts == ["你好，我在。"]
    assert player.played
    assert runner.history.request_messages()[-1]["content"] == "你好，我在。"
    await close_runner(service, session, runner)


@pytest.mark.asyncio
async def test_second_turn_keeps_history_without_new_wake(tmp_path):
    service, session, runner, sink, llm, _tts, _player = await make_runner(
        tmp_path, answer="第一轮回答。", asr_text="第一句"
    )
    llm.answers = ["第一轮回答。", "第二轮回答。"]
    runner.feed(pcm(0.3, 3000) + pcm(0.3))
    await asyncio.wait_for(sink.done.wait(), timeout=2)
    sink.done.clear()
    runner.feed(pcm(0.3, 3000) + pcm(0.3))
    await asyncio.wait_for(sink.done.wait(), timeout=2)
    user_turns = [e for e in json_events(sink) if e["type"] == "user_text"]
    assert len(user_turns) == 2
    assert len(llm.calls) == 2
    assert llm.calls[1][0]["role"] == "system"
    assert any(m.get("content") == "第一轮回答。" for m in llm.calls[1])
    await close_runner(service, session, runner)


@pytest.mark.asyncio
async def test_end_phrase_ends_session(tmp_path):
    service, session, runner, sink, llm, _tts, _player = await make_runner(
        tmp_path, answer="不该调用。", asr_text="再见"
    )
    runner.feed(pcm(0.3, 3000) + pcm(0.3))
    await asyncio.wait_for(runner.ended.wait(), timeout=2)
    assert runner.end_reason == "user_end"
    assert any(
        e["type"] == "assistant_done" and "再见" in e.get("text", "")
        for e in json_events(sink)
    )
    assert llm.calls == []
    await close_runner(service, session, runner)


@pytest.mark.asyncio
async def test_idle_timeout_ends_session(tmp_path):
    service, session, runner, _sink, _llm, _tts, _player = await make_runner(tmp_path)
    runner.config = ConversationConfig(idle_s=0.1, turn_end_ms=200)
    await asyncio.wait_for(runner.ended.wait(), timeout=2)
    assert runner.end_reason == "idle"
    await close_runner(service, session, runner)
