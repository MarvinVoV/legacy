"""pytest fixtures：FakeMQTT + FastAPI TestClient（lifespan 已跑）"""

import pytest
from fastapi.testclient import TestClient


class FakeMQTT:
    """假的 MQTT 客户端：记录发布，默认已连接"""

    def __init__(self):
        self.is_connected = True
        self.published: list[tuple[str, object]] = []

    async def publish(self, topic, payload, qos=0):
        self.published.append((topic, payload))


@pytest.fixture()
def app():
    from app.main import app as _app

    return _app


@pytest.fixture()
def client(app):
    with TestClient(app) as c:
        # 用假 MQTT 替换 lifespan 创建的真实客户端，便于断言
        c.app.state.mqtt_client = FakeMQTT()
        yield c
