import json

from scripts.voice_acceptance_report import summarize


def test_quantiles_count_only_valid_samples_and_do_not_claim_acoustic_measurement():
    lines = []
    for turn in range(1, 21):
        lines.append(
            f"2026-11-12 18:00:{turn:02} voice_metric "
            + json.dumps(
                {
                    "name": "barge_stop_ack",
                    "turn": turn,
                    "server_ack_ms": turn * 10,
                    "local_dma_drain_ms": 100 + turn,
                }
            )
        )
    lines += [
        lines[0],
        "x voice_metric {bad",
        "x voice_metric []",
        'x voice_metric {"name":"barge_stop_ack","turn":21,"server_ack_ms":true}',
        "voice timing turn=1 asr_ms=250",
        "voice timing turn=1 tts_ms=400",
    ]
    report = summarize("\n".join(lines))
    assert report["server_barge_event_to_stop_ack"] == {
        "samples": 20,
        "p50_ms": 100,
        "p95_ms": 190,
        "max_ms": 200,
    }
    assert report["enough_ack_samples"]
    assert report["asr_request"]["p50_ms"] == 250
    assert report["tts_request"]["p50_ms"] == 400
    assert "not microphone-measured" in report["limits"][0]


def test_empty_or_invalid_logs_cannot_pass_sample_requirement():
    report = summarize(
        'voice_metric {"name":"barge_stop_ack","turn":1,'
        '"server_ack_ms":NaN,"local_dma_drain_ms":-1}'
    )
    assert not report["enough_ack_samples"]
    assert report["server_barge_event_to_stop_ack"]["samples"] == 0
