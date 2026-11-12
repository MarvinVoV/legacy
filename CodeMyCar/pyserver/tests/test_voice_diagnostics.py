import json
import wave

from app.services.voice_diagnostics import InterruptionCapture


def test_capture_has_fixed_time_and_byte_budget(tmp_path):
    now = [10.0]
    path = tmp_path / "uplink.wav"
    capture = InterruptionCapture(path, seconds=1, clock=lambda: now[0])
    capture.feed(b"\x01\x00" * 20000, "interrupting")
    assert len(capture.pcm) == 32000
    capture.feed(b"\x02\x00" * 100, "thinking")
    assert capture.closed and not capture.pcm
    with wave.open(str(path), "rb") as wav:
        assert wav.getnframes() == 16000 and wav.getframerate() == 16000
    trace = json.loads(path.with_suffix(".json").read_text())
    assert trace["frames"] == [
        {"arrival_ms": 0.0, "offset": 0, "bytes": 32000, "state": "interrupting"}
    ]


def test_capture_stops_on_wall_clock_even_if_uplink_stalls(tmp_path):
    now = [10.0]
    capture = InterruptionCapture(tmp_path / "uplink.wav", seconds=1, clock=lambda: now[0])
    capture.feed(b"\0" * 640, "listening")
    now[0] = 11.1
    capture.feed(b"\0" * 640, "thinking")
    assert capture.closed
    with wave.open(str(capture.path), "rb") as wav:
        assert wav.getnframes() == 320


def test_unwritable_diagnostic_target_does_not_break_voice_cleanup(tmp_path):
    blocked = tmp_path / "not-a-directory"
    blocked.write_text("occupied")
    capture = InterruptionCapture(blocked / "uplink.wav")
    capture.feed(b"\0" * 640, "listening")
    capture.close()
    assert capture.closed and not capture.pcm
