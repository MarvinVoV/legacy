"""语音文本 -> 固定意图。

只识别白名单命令；未知文本一律返回 None，绝不猜测。
数字同时兼容中文（三十）和阿拉伯（30），因为百度 ASR 两种都可能返回。
"""

from __future__ import annotations

import re
from dataclasses import dataclass

_CN_DIGITS = {
    "零": 0,
    "一": 1,
    "二": 2,
    "两": 2,
    "三": 3,
    "四": 4,
    "五": 5,
    "六": 6,
    "七": 7,
    "八": 8,
    "九": 9,
}


def _cn_to_int(text: str) -> int | None:
    if not text:
        return None
    if "百" in text:
        left, _, right = text.partition("百")
        hundreds = _CN_DIGITS.get(left, 1) if left else 1
        rest = _cn_to_int(right) if right else 0
        return hundreds * 100 + (rest or 0)
    if "十" in text:
        left, _, right = text.partition("十")
        tens = _CN_DIGITS.get(left, 1) if left else 1
        ones = _CN_DIGITS.get(right, 0) if right else 0
        return tens * 10 + ones
    if len(text) == 1:
        return _CN_DIGITS.get(text)
    return None


def normalize_numbers(text: str) -> str:
    def repl(match: re.Match[str]) -> str:
        value = _cn_to_int(match.group())
        return str(value) if value is not None else match.group()

    return re.sub(r"[零一二两三四五六七八九十百]+", repl, text)


def _clean(text: str) -> str:
    return text.strip().strip("。！？，,.!?；;：: \t\r\n")


@dataclass(frozen=True)
class VoiceIntent:
    kind: str  # stop / cancel / forward / backward / turn_left / turn_right
    distance_mm: int = 0
    angle_deg: int = 0
    text: str = ""


def parse_intent(raw_text: str) -> VoiceIntent | None:
    """解析白名单命令；无法确定时返回 None。"""
    if not raw_text:
        return None
    text = normalize_numbers(_clean(raw_text))
    if not text:
        return None

    if any(word in text for word in ("取消", "算了", "不走了")):
        return VoiceIntent("cancel", text=text)
    if any(word in text for word in ("停止", "停车", "停下", "停")):
        return VoiceIntent("stop", text=text)

    left = any(word in text for word in ("左转", "向左转"))
    right = any(word in text for word in ("右转", "向右转"))
    if left and right:
        return None
    if left or right:
        match = re.search(r"(\d+(?:\.\d+)?)\s*度", text)
        if not match:
            return None
        angle = int(float(match.group(1)))
        if angle <= 0:
            return None
        return VoiceIntent("turn_left" if left else "turn_right", angle_deg=angle, text=text)

    forward = any(word in text for word in ("前进", "向前", "往前"))
    backward = any(word in text for word in ("后退", "向后", "倒退", "倒回"))
    if forward and backward:
        return None
    if forward or backward:
        match = re.search(r"(\d+(?:\.\d+)?)\s*(米|厘米|cm|m)", text, re.IGNORECASE)
        if not match:
            return None
        value = float(match.group(1))
        unit = match.group(2).lower()
        distance_mm = int(value * (1000 if unit in ("米", "m") else 10))
        if distance_mm <= 0:
            return None
        return VoiceIntent(
            "forward" if forward else "backward",
            distance_mm=distance_mm,
            text=text,
        )

    return None
