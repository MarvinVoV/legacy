import json

from scripts.voice_probe_report import summarize


def test_report_keeps_mixed_room_labels_and_unfinished_phase_separate(tmp_path):
    events = [
        {"event": "stage_start", "stage": "quiet", "utc": "2026-11-12T10:00:00+00:00"},
        {"event": "interruption", "server_ack_ms": 120, "local_dma_drain_ms": 130},
        {
            "event": "stage_complete",
            "utc": "2026-11-12T10:10:00+00:00",
            "result": {"interruptions": 1},
        },
        {"event": "stage_start", "stage": "quiet", "utc": "2026-11-12T10:10:00+00:00"},
    ]
    (tmp_path / "events.jsonl").write_text("\n".join(json.dumps(e) for e in events))
    (tmp_path / "annotations.jsonl").write_text(
        json.dumps({"phase_index": 1, "label": "mixed_home"})
    )
    h = [
        {"utc": "2026-11-12T10:01:00+00:00", "heap": 52000, "heap_min": 37000, "uptime_s": 60},
        {"utc": "2026-11-12T10:11:00+00:00", "heap": 53000, "heap_min": 38000, "uptime_s": 10},
    ]
    (tmp_path / "health.jsonl").write_text("\n".join(json.dumps(e) for e in h))
    report = summarize(tmp_path)
    assert report["phases"][0]["annotation"]["label"] == "mixed_home"
    assert report["phases"][0]["zero_barge_events"] is False
    assert report["phases"][0]["health"]["heap_max"] == 52000
    assert report["phases"][1]["zero_barge_events"] is None
    assert not report["phases"][1]["complete"]


def test_serial_report_preserves_cumulative_counters_and_detects_real_errors(tmp_path):
    p = tmp_path / "serial"
    p.write_text(
        "old codec read failed\nHOST_COMMAND s\nreplaced_frames=2 clips=1/0\n"
        "replaced_frames=3 clips=1/0\ncodec write failed\nESP-ROM:\n"
    )
    report = summarize(tmp_path, p)["serial_since_last_manual_start"]
    assert report["codec_read_errors"] == 0 and report["codec_write_errors"] == 1
    assert report["boot_markers"] == 1 and report["replaced_frames_first_last"] == [2, 3]
    assert report["mic_ref_clips_first_last"] == [(1, 0), (1, 0)]


def test_known_human_utterance_is_not_counted_as_a_confirmed_false_alarm(tmp_path):
    events = [
        {"event": "stage_start", "stage": "quiet", "utc": "2026-11-12T10:00:00+00:00"},
        {"event": "interruption", "utc": "2026-11-12T10:01:00+00:00"},
        {"event": "interruption", "utc": "2026-11-12T10:02:00+00:00"},
        {"event": "stage_complete", "utc": "2026-11-12T10:10:00+00:00", "result": {}},
    ]
    (tmp_path / "events.jsonl").write_text("\n".join(json.dumps(e) for e in events))
    (tmp_path / "annotations.jsonl").write_text(
        json.dumps(
            {
                "phase_index": 1,
                "event_utc": "2026-11-12T10:01:00+00:00",
                "classification": "human_speech",
            }
        )
    )
    phase = summarize(tmp_path)["phases"][0]
    assert phase["barge_events"] == 2 and phase["human_speech_events"] == 1
    assert phase["confirmed_false_events"] == 0 and phase["unclassified_events"] == 1


def test_capture_rate_is_wall_throughput_not_a_cpu_utilization_claim(tmp_path):
    p = tmp_path / "serial"
    p.write_text(
        "HOST_COMMAND s\ncapture_perf ok_frames=80000 wall_us=5000000 "
        "read_us=3000000 feed_us=1900000 active=1\n"
        "capture_perf ok_frames=80000 wall_us=0 read_us=0 feed_us=0 active=1\n"
    )
    timing = summarize(tmp_path, p)["serial_since_last_manual_start"]["capture_wall_timing_windows"]
    assert len(timing) == 1 and timing[0]["handled_samples_per_second"] == 16000
    assert timing[0]["active_at_window_end"] and timing[0]["read_wall_us"] == 3000000
