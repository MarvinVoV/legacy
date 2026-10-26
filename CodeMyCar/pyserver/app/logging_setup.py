"""文件日志：按大小轮转，超过备份数自动删除最旧文件。"""

import logging
from logging.handlers import RotatingFileHandler
from pathlib import Path

from app.config import Settings

_HANDLER_NAME = "cmcar-file"
_FORMAT = "%(asctime)s [%(name)s] %(levelname)s: %(message)s"
# uvicorn.error 没有自己的 handler，会向上冒泡到 uvicorn；
# 给两者都挂同一个 handler 会导致每条 uvicorn.error 日志写两遍。
_UVICORN_LOGGERS = ("uvicorn", "uvicorn.access")


def build_file_handler(settings: Settings) -> RotatingFileHandler | None:
    """创建轮转文件 handler；log_file 为空时返回 None（禁用文件日志）。"""
    if not settings.log_file:
        return None

    log_dir = Path(settings.log_dir)
    log_dir.mkdir(parents=True, exist_ok=True)

    handler = RotatingFileHandler(
        log_dir / settings.log_file,
        maxBytes=settings.log_max_bytes,
        backupCount=settings.log_backup_count,
        encoding="utf-8",
    )
    handler.setFormatter(logging.Formatter(_FORMAT))
    handler.setLevel(getattr(logging, settings.log_level.upper(), logging.INFO))
    handler.name = _HANDLER_NAME
    return handler


def setup_file_logging(settings: Settings) -> RotatingFileHandler | None:
    """把文件 handler 挂到 root 与 uvicorn 的 logger；重复调用不会重复挂载。"""
    root = logging.getLogger()
    for existing in root.handlers:
        if getattr(existing, "name", "") == _HANDLER_NAME:
            return existing  # type: ignore[return-value]

    handler = build_file_handler(settings)
    if handler is None:
        return None

    targets = [root, *(logging.getLogger(name) for name in _UVICORN_LOGGERS)]
    for logger in targets:
        logger.addHandler(handler)
    return handler
