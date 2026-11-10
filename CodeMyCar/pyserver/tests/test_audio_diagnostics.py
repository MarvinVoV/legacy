import math
import struct
from dataclasses import replace
from pathlib import Path

import pytest

from app.config import Settings
from app.services.audio_metrics import measure_pcm16
from tests.test_voice_ws import make_service, pcm, start_frame


def test_metrics_distinguish_dc_from_speech_energy():
    result = measure_pcm16(struct.pack("<1000h", *([1000] * 1000)))
    assert result.rms == result.dc_offset == 1000
    assert result.ac_rms == 0
    assert result.clipped_samples == 0


def test_metrics_report_both_clipping_limits_and_duration():
    result = measure_pcm16(struct.pack("<4h", -32768, 32767, -100, 100), 1000)
    assert result.samples == 4 and result.duration_ms == 4
    assert result.peak == 32768
    assert result.clipped_samples == 2 and result.clip_fraction == 0.5


def test_silence_and_empty_metrics_are_json_safe():
    for data in (b"", b"\0" * 640):
        assert all(math.isfinite(v) for v in measure_pcm16(data).as_dict().values())


@pytest.mark.parametrize("data,rate", [(b"\0", 16000), (b"\0\0", 0), (b"", -1)])
def test_rejects_invalid_pcm(data, rate):
    with pytest.raises(ValueError):
        measure_pcm16(data, rate)


def test_archiving_is_opt_in():
    assert Settings(_env_file=None).voice_archive_continuous_segments is False


def test_offline_diagnostic_archives_one_utterance_without_cloud(client, tmp_path):
    service = make_service(tmp_path, None)
    service.config = replace(service.config, archive_continuous_segments=True)
    client.app.state.voice_service = service
    with client.websocket_connect("/api/v1/voice/stream") as ws:
        ws.send_text(start_frame(continuous=True, one_shot=True))
        assert ws.receive_json()["type"] == "ready"
        ws.send_bytes(pcm(0.3, 3000) + pcm(0.3))
        assert ws.receive_json()["type"] == "recognizing"
        result = ws.receive_json()
        assert result["type"] == "final" and result["text"] is None
        assert not result["accepted"] and not result["execution_enabled"]
        assert Path(result["wav"]).is_file()
        assert result["audio_metrics"]["rms"] > 0
        assert result["audio_metrics"]["clipped_samples"] == 0
        ws.send_text('{"type":"end"}')
        assert ws.receive_json()["type"] == "end"
    assert len(list(tmp_path.glob("*.wav"))) == 1


def test_default_continuous_mode_does_not_record_audio(client, tmp_path):
    client.app.state.voice_service = make_service(tmp_path, None)
    with client.websocket_connect("/api/v1/voice/stream") as ws:
        ws.send_text(start_frame(continuous=True, one_shot=True))
        assert ws.receive_json()["type"] == "ready"
        ws.send_bytes(pcm(0.3, 3000) + pcm(0.3))
        assert ws.receive_json()["type"] == "recognizing"
        final = ws.receive_json()
        assert final["wav"] is None and "audio_metrics" not in final
        ws.send_text('{"type":"end"}')
        assert ws.receive_json()["type"] == "end"
    assert not list(tmp_path.glob("*.wav"))


def test_diagnostic_archive_limit_does_not_stop_audio_processing(client, tmp_path):
    service = make_service(tmp_path, None)
    service.config = replace(service.config, archive_continuous_segments=True, archive_max_segments=1)
    client.app.state.voice_service = service
    for index in range(2):
        with client.websocket_connect("/api/v1/voice/stream") as ws:
            ws.send_text(start_frame(continuous=True, one_shot=True))
            assert ws.receive_json()["type"] == "ready"
            ws.send_bytes(pcm(0.3, 3000) + pcm(0.3))
            assert ws.receive_json()["type"] == "recognizing"
            result = ws.receive_json()
            assert result["audio_metrics"]["samples"] > 0
            assert bool(result["wav"]) == (index == 0)
            ws.send_text('{"type":"end"}')
            assert ws.receive_json()["type"] == "end"
    assert len(list(tmp_path.glob("*.wav"))) == 1
