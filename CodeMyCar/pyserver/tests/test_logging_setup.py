"""文件日志：轮转与自动清理。"""

import logging
from pathlib import Path

from app.config import Settings
from app.logging_setup import build_file_handler, setup_file_logging


def make_settings(tmp_path: Path, **overrides) -> Settings:
    base = dict(
        log_dir=str(tmp_path),
        log_file="server.log",
        log_max_bytes=400,
        log_backup_count=2,
        log_level="INFO",
    )
    base.update(overrides)
    return Settings(**base)


def test_rotating_file_handler_cleans_old_backups(tmp_path):
    handler = build_file_handler(make_settings(tmp_path))
    assert handler is not None

    logger = logging.getLogger("test.rotation")
    logger.setLevel(logging.INFO)
    logger.propagate = False
    logger.addHandler(handler)
    try:
        for i in range(300):
            logger.info("line %03d %s", i, "x" * 60)
    finally:
        logger.removeHandler(handler)
        handler.close()

    names = {p.name for p in tmp_path.iterdir()}
    assert "server.log" in names
    assert "server.log.1" in names
    assert "server.log.2" in names
    assert "server.log.3" not in names  # backupCount 之外的旧文件被自动删除


def test_empty_log_file_disables_file_logging(tmp_path):
    assert build_file_handler(make_settings(tmp_path, log_file="")) is None


def test_setup_is_idempotent(tmp_path):
    settings = make_settings(tmp_path)
    first = setup_file_logging(settings)
    second = setup_file_logging(settings)
    third = setup_file_logging(settings)
    assert first is second is third
    root_handlers = [
        h for h in logging.getLogger().handlers if getattr(h, "name", "") == "cmcar-file"
    ]
    try:
        assert len(root_handlers) == 1
    finally:
        for handler in root_handlers:
            logging.getLogger().removeHandler(handler)
            for name in ("uvicorn", "uvicorn.access"):
                logging.getLogger(name).removeHandler(handler)
            handler.close()


def test_uvicorn_error_logged_once(tmp_path):
    """uvicorn.error 会冒泡到 uvicorn，不能因为两边都挂 handler 而写两遍"""
    settings = make_settings(tmp_path)
    uvicorn_logger = logging.getLogger("uvicorn")
    uvicorn_error = logging.getLogger("uvicorn.error")
    old_propagate = uvicorn_logger.propagate
    old_level = uvicorn_logger.level
    old_error_level = uvicorn_error.level
    uvicorn_logger.propagate = False  # 模拟 uvicorn 的 LOGGING_CONFIG
    uvicorn_logger.setLevel(logging.INFO)
    uvicorn_error.setLevel(logging.INFO)

    handler = setup_file_logging(settings)
    try:
        uvicorn_error.info("dup-check-marker")
        assert handler is not None
        handler.flush()
        content = (tmp_path / "server.log").read_text(encoding="utf-8")
        assert content.count("dup-check-marker") == 1
    finally:
        if handler is not None:
            logging.getLogger().removeHandler(handler)
            for name in ("uvicorn", "uvicorn.access"):
                logging.getLogger(name).removeHandler(handler)
            handler.close()
        uvicorn_logger.propagate = old_propagate
        uvicorn_logger.setLevel(old_level)
        uvicorn_error.setLevel(old_error_level)
