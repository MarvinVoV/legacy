"""语音意图解析测试：中文/阿拉伯数字、白名单、拒绝歧义。"""

import pytest

from app.services.voice_intent import normalize_numbers, parse_intent


@pytest.mark.parametrize(
    "text,kind,distance_mm,angle_deg",
    [
        ("前进一米。", "forward", 1000, 0),
        ("向前走两米", "forward", 2000, 0),
        ("前进3米", "forward", 3000, 0),
        ("前进一百厘米", "forward", 1000, 0),
        ("后退一米", "backward", 1000, 0),
        ("倒回 50 厘米", "backward", 500, 0),
        ("停止。", "stop", 0, 0),
        ("停车", "stop", 0, 0),
        ("停下", "stop", 0, 0),
        ("左转三十度。", "turn_left", 0, 30),
        ("向右转30度", "turn_right", 0, 30),
        ("向左转90度", "turn_left", 0, 90),
        ("取消", "cancel", 0, 0),
        ("算了", "cancel", 0, 0),
    ],
)
def test_parses_whitelisted_commands(text, kind, distance_mm, angle_deg):
    intent = parse_intent(text)
    assert intent is not None
    assert intent.kind == kind
    assert intent.distance_mm == distance_mm
    assert intent.angle_deg == angle_deg


@pytest.mark.parametrize(
    "text",
    [
        "",
        "你好",
        "前进",
        "左转",
        "前进一米后退一米",
        "左转三十度右转三十度",
        "open the door",
        "前进很多米",
    ],
)
def test_rejects_unknown_or_ambiguous(text):
    assert parse_intent(text) is None


def test_normalize_numbers():
    assert normalize_numbers("左转三十度") == "左转30度"
    assert normalize_numbers("十五度") == "15度"
    assert normalize_numbers("一百厘米") == "100厘米"
    assert normalize_numbers("没有数字") == "没有数字"
