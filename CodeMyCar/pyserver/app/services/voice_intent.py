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
    digit = "一二两三四五六七八九"
    if "百" in text:
        m = re.fullmatch(f"([{digit}])百(零[{digit}]|[{digit}]?十[{digit}]?)?", text)
        if not m:
            return None
        rest = _cn_to_int(m[2].removeprefix("零")) if m[2] else 0
        return _CN_DIGITS[m[1]] * 100 + rest if rest is not None else None
    if "十" in text:
        m = re.fullmatch(f"([{digit}])?十([{digit}])?", text)
        if not m:
            return None
        return (_CN_DIGITS[m[1]] if m[1] else 1) * 10 + (_CN_DIGITS[m[2]] if m[2] else 0)
    return _CN_DIGITS.get(text) if len(text) == 1 else None


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
    """Accept exactly one command; never extract executable substrings."""
    if not raw_text:
        return None
    text = normalize_numbers(_clean(raw_text))
    if re.fullmatch(r"(?:取消|算了|不走了)", text):
        return VoiceIntent("cancel", text=text)
    if re.fullmatch(r"(?:停止|停车|停下|停)", text):
        return VoiceIntent("stop", text=text)
    turn = re.fullmatch(r"(?:向)?(左|右)转\s*(\d+)\s*度", text)
    if turn:
        angle = int(turn[2])
        return (
            VoiceIntent(
                "turn_left" if turn[1] == "左" else "turn_right", angle_deg=angle, text=text
            )
            if angle > 0
            else None
        )
    move = re.fullmatch(
        r"(前进|向前走?|往前走?|后退|后腿|向后走?|倒退|倒回)\s*(\d+(?:\.\d+)?)\s*(厘米|米|cm|m)",
        text,
        re.IGNORECASE,
    )
    if move:
        from decimal import Decimal

        value = Decimal(move[2]) * (1000 if move[3].lower() in ("米", "m") else 10)
        if value <= 0 or value != value.to_integral_value():
            return None
        return VoiceIntent(
            "forward" if move[1] in ("前进", "向前", "向前走", "往前", "往前走") else "backward",
            distance_mm=int(value),
            text=text,
        )
    return None
