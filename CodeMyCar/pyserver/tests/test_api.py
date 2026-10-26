"""API 测试：命令下发、设备状态、API Key 鉴权"""

from fastapi.testclient import TestClient


def test_send_command_success(client):
    body = {
        "seq": 1,
        "cmd": "motion",
        "motion": {"mode": 2, "params": {"linear_vel": 500, "angular_vel": 100}},
    }
    r = client.post("/api/v1/devices/DEV_1/commands", json=body)
    assert r.status_code == 200
    data = r.json()
    assert data["success"] is True
    # 假 MQTT 记录了一次发布，topic 为统一契约 cmd/{device_id}/exec
    fake = client.app.state.mqtt_client
    assert len(fake.published) == 1
    topic, payload = fake.published[0]
    assert topic == "cmd/DEV_1/exec"
    assert payload["motion"]["mode"] == 2


def test_send_command_mqtt_disconnected(client):
    client.app.state.mqtt_client.is_connected = False
    body = {"seq": 1, "cmd": "motion", "motion": {"mode": 0, "params": {}}}
    r = client.post("/api/v1/devices/DEV_1/commands", json=body)
    assert r.status_code == 200
    assert r.json()["success"] is False


def test_send_command_invalid_body(client):
    # mode 越界
    body = {"seq": 1, "cmd": "motion", "motion": {"mode": 99, "params": {}}}
    r = client.post("/api/v1/devices/DEV_1/commands", json=body)
    assert r.status_code == 422


def test_list_devices_empty(client):
    r = client.get("/api/v1/devices")
    assert r.status_code == 200
    data = r.json()
    assert data["success"] is True
    assert data["data"] == []


def test_device_status_unknown_offline(client):
    r = client.get("/api/v1/devices/NOPE/status")
    assert r.status_code == 200
    assert r.json()["data"]["online"] is False


def test_root_public_no_key(client):
    r = client.get("/")
    assert r.status_code == 200
    assert r.json()["status"] == "running"


def test_api_key_required(app):
    import app.main as m

    old = m.settings.api_key
    m.settings.api_key = "secret"
    try:
        with TestClient(app) as c:
            # 无 key → 401
            assert c.get("/api/v1/devices").status_code == 401
            # 错 key → 401
            assert c.get("/api/v1/devices", headers={"X-API-Key": "bad"}).status_code == 401
            # 正确 key → 200
            assert c.get("/api/v1/devices", headers={"X-API-Key": "secret"}).status_code == 200
            # 公开路径无需 key
            assert c.get("/").status_code == 200
    finally:
        m.settings.api_key = old
