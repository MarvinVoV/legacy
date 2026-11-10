"""能量 VAD 单元测试：切段、最短语音、最大段长、flush。"""

import math
import struct

import pytest

from app.services.vad import EnergyVad, VadConfig


def pcm(seconds: float, amp: float = 0.0, freq: float = 440.0) -> bytes:
    n = int(16000 * seconds)
    if amp == 0:
        return b"\x00\x00" * n
    values = [int(amp * math.sin(2 * math.pi * freq * i / 16000)) for i in range(n)]
    return struct.pack(f"<{n}h", *values)


def config(**kwargs) -> VadConfig:
    base = {
        "start_rms": 300,
        "end_rms": 200,
        "min_speech_ms": 100,
        "end_silence_ms": 200,
        "pre_roll_ms": 100,
        "max_segment_ms": 10000,
    }
    base.update(kwargs)
    return VadConfig(**base)


def feed_all(vad: EnergyVad, data: bytes, chunk: int = 100) -> list[bytes]:
    out: list[bytes] = []
    for i in range(0, len(data), chunk):
        out.extend(vad.feed(data[i : i + chunk]))
    return out


def test_emits_one_segment_with_pre_roll_and_tail():
    vad = EnergyVad(config())
    audio = pcm(0.3) + pcm(0.8, 3000) + pcm(0.5)
    segments = feed_all(vad, audio)
    assert len(segments) == 1
    duration = len(segments[0]) / 2 / 16000
    # 100ms pre-roll + 800ms 语音 + 200ms 尾部静音 ≈ 1.1s
    assert 1.0 <= duration <= 1.3
    # 段首应是 pre-roll 的静音
    assert segments[0][:100] == b"\x00" * 100


def test_short_blip_is_discarded():
    vad = EnergyVad(config())
    audio = pcm(0.05, 3000) + pcm(0.5)
    assert feed_all(vad, audio) == []
    assert vad.flush() == []


def test_two_utterances_produce_two_segments():
    vad = EnergyVad(config())
    audio = pcm(0.2) + pcm(0.6, 3000) + pcm(0.5) + pcm(0.6, 3000) + pcm(0.4)
    segments = feed_all(vad, audio)
    assert len(segments) == 2
    for segment in segments:
        assert 0.6 <= len(segment) / 2 / 16000 <= 1.2


def test_max_segment_forces_split():
    vad = EnergyVad(config(max_segment_ms=1000))
    audio = pcm(3.0, 3000)
    segments = feed_all(vad, audio)
    assert len(segments) >= 2
    for segment in segments:
        assert len(segment) / 2 / 16000 <= 1.1


def test_flush_emits_open_segment():
    vad = EnergyVad(config())
    assert feed_all(vad, pcm(0.5, 3000)) == []
    segments = vad.flush()
    assert len(segments) == 1
    assert len(segments[0]) / 2 / 16000 >= 0.5


def test_silence_only_produces_nothing():
    vad = EnergyVad(config())
    assert feed_all(vad, pcm(2.0)) == []
    assert vad.flush() == []


def test_arbitrary_chunk_boundaries():
    vad = EnergyVad(config())
    audio = pcm(0.2) + pcm(0.6, 3000) + pcm(0.5)
    segments = feed_all(vad, audio, chunk=7)
    assert len(segments) == 1


@pytest.fixture
def speech_classifier(monkeypatch):
    """An over-eager classifier isolates the energy contract from model heuristics."""
    import webrtcvad

    class Classifier:
        calls = 0

        def is_speech(self, frame, rate):
            self.calls += 1
            return True

    detector = Classifier()
    monkeypatch.setattr(webrtcvad, "Vad", lambda mode: detector)
    return detector


def test_webrtc_does_not_treat_dc_offset_as_speech(speech_classifier):
    vad = EnergyVad(config(backend="webrtc"))
    dc = struct.pack("<h", 2000) * 16000
    assert feed_all(vad, dc) == []
    assert vad.flush() == []
    assert speech_classifier.calls == 50  # adaptation still sees every 20ms frame


def test_webrtc_ends_phrase_on_low_energy_background(speech_classifier):
    vad = EnergyVad(config(backend="webrtc"))
    background = struct.pack("<h", 800) * 16000
    segments = feed_all(vad, pcm(.6, 3000) + background)
    assert len(segments) == 1
    assert len(segments[0]) / 32000 < 1.0


def test_webrtc_energy_floor_can_be_lowered_for_quiet_speech(speech_classifier):
    vad = EnergyVad(config(backend="webrtc", start_rms=30, end_rms=20))
    segments = feed_all(vad, pcm(.6, 100) + pcm(.5))
    assert len(segments) == 1
