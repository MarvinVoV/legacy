# MQTT Ping/Pong 链路自检 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 让 pyserver 能手动发一条 ping，ESP32 收到后直接回 pong，用于在「ESP32 上电 + pyserver 已启动」时自检 MQTT 上下行，不依赖 STM32。

**Architecture:** 复用现有下行 topic `cmd/{device_id}/exec`；ESP32 在 `process_downlink()` 中、进入 motion 解析之前拦截 `{"cmd":"ping","seq":N}`，用 `snprintf` 构造 `{"seq":N,"pong":true,"uptime_s":U,"heap":H}` 发到新 topic `data/{device_id}/pong`，不经过 UART。pyserver 订阅 `data/+/pong`，按 `(device_id, seq)` 唤醒等待中的请求并计算 RTT，3 秒超时。

**Tech Stack:** ESP-IDF v6.1 (C11, cJSON, esp_timer) / pyserver (Python 3.11+, FastAPI, paho-mqtt, pytest-asyncio) / host 侧 `serial_pkt_protocol` ctest。

**Spec:** 对话中已确认（2026-10-25）：手动连通性自检；3s 超时；新 topic `pong`；不复用 ACK；不依赖 STM32。

## Global Constraints

- 目标 ESP32-C6，无 FPU：pong 只有整数，不出现 `%f`。
- 热路径零 malloc：pong 用栈上 `char[96]` + `snprintf`，解析 ping 用 cJSON（低频下行，允许）。
- 所有 topic 只含一个 `device_id` 层级：`data/{device_id}/pong`。
- `bridge_codec.{c,h}` 在 `esp32_project/components/bridge/` 与 `serial_pkt_protocol/` 是**镜像副本**，两处必须一致。
- pyserver 只处理 JSON；不引入二进制协议。
- 不改动 motion 命令、ACK（`data/{id}/ack`）、心跳、STM32/UART 逻辑。
- 超时默认 3.0s，可在 `PingService` 构造参数注入（测试用）。
- 新增代码注释用中文，与现有代码一致。

## Review Focus

- 收到未知/过期 `seq` 的 pong：必须忽略且不崩溃，不能误唤醒别的 ping。
- 并发多个 ping：每个等待者只被自己的 seq 唤醒；seq 回绕（0→255）不得与在途 seq 碰撞。
- MQTT 未连接时调用 ping：立即返回明确失败，不得挂起 3 秒。
- 畸形 pong（非 JSON、缺字段、`pong!=true`）：忽略，不影响后续 ping。
- C 侧 `snprintf` 缓冲区过小：返回 0，不截断、不越界。

---

## Task 1: Codec 辅助函数 + PONG topic（含 host 镜像与测试）

**Files:**
- Modify: `CodeMyCar/esp32_project/components/common/include/cmcar_topics.h`
- Modify: `CodeMyCar/esp32_project/components/bridge/include/bridge_codec.h`
- Modify: `CodeMyCar/esp32_project/components/bridge/src/bridge_codec.c`
- Modify: `serial_pkt_protocol/include/bridge_codec.h`（镜像）
- Modify: `serial_pkt_protocol/src/bridge_codec.c`（镜像）
- Test: `serial_pkt_protocol/tests/test_bridge_codec.c`

**Interfaces:**
- Produces:
  - `bool bridge_json_is_ping(const char* json, size_t len, uint8_t* out_seq);`
    - 是 ping（`cmd == "ping"`，大小写敏感）→ `true`，`*out_seq = seq & 0xFF`（缺失默认 0）；否则 `false` 且不改 `*out_seq`。
  - `int bridge_pong_to_json(uint8_t seq, unsigned long long uptime_s, unsigned long heap, char* out, size_t out_size);`
    - 成功 → 写入 `{"seq":<n>,"pong":true,"uptime_s":<u>,"heap":<h>}`，返回长度（不含 `\0`）；缓冲区不足 → 0。
  - `#define CMCAR_TOPIC_PONG "data/" CMCAR_DEVICE_ID "/pong"`

- [ ] **Step 1: 先创建功能分支**

```bash
cd /Users/sunyameng/github/legacy
git checkout -b feat/mqtt-ping-pong
```

- [ ] **Step 2: 在 host 测试中写失败用例**

在 `serial_pkt_protocol/tests/test_bridge_codec.c` 的 `main()` 末尾（`return` 之前）追加：

```c
    /* ping/pong */
    uint8_t pseq = 0xAA;
    const char* j_ping = "{\"cmd\":\"ping\",\"seq\":42}";
    CHECK(bridge_json_is_ping(j_ping, strlen(j_ping), &pseq) == true, "ping detected");
    CHECK(pseq == 42, "ping seq=42");

    pseq = 0xAA;
    CHECK(bridge_json_is_ping(j_diff, strlen(j_diff), &pseq) == false, "motion is not ping");
    CHECK(pseq == 0xAA, "non-ping leaves seq untouched");

    const char* j_ping_noseq = "{\"cmd\":\"ping\"}";
    pseq = 0xAA;
    CHECK(bridge_json_is_ping(j_ping_noseq, strlen(j_ping_noseq), &pseq) == true, "ping without seq");
    CHECK(pseq == 0, "missing seq defaults to 0");

    const char* j_ping_wrap = "{\"cmd\":\"ping\",\"seq\":300}";
    pseq = 0;
    CHECK(bridge_json_is_ping(j_ping_wrap, strlen(j_ping_wrap), &pseq) == true, "seq>255 still ping");
    CHECK(pseq == 44, "seq 300 & 0xFF == 44");

    CHECK(bridge_json_is_ping("not-json", 8, &pseq) == false, "invalid json is not ping");

    char pong[96];
    int pn = bridge_pong_to_json(42, 123ULL, 80000UL, pong, sizeof(pong));
    CHECK(pn > 0, "pong json built");
    CHECK(strcmp(pong, "{\"seq\":42,\"pong\":true,\"uptime_s\":123,\"heap\":80000}") == 0,
          "pong json exact");
    CHECK(bridge_pong_to_json(1, 1ULL, 1UL, pong, 8) == 0, "tiny buffer rejected");
```

- [ ] **Step 3: 运行测试，确认失败**

```bash
cd /Users/sunyameng/github/legacy/serial_pkt_protocol
cmake -B build && cmake --build build
cd build/tests && ctest --output-on-failure
```

Expected: 编译失败（`bridge_json_is_ping` / `bridge_pong_to_json` 未声明）。

- [ ] **Step 4: 实现（先改 ESP32 源，再复制到镜像）**

`components/bridge/include/bridge_codec.h` 追加：

```c
/**
 * @brief 下行：判断是否为 ping 命令并取 seq
 * @param out_seq [out] seq（& 0xFF）；非 ping 时不修改
 * @return true 是 ping，false 不是/解析失败
 */
bool bridge_json_is_ping(const char* json, size_t len, uint8_t* out_seq);

/**
 * @brief 构建 pong JSON：{"seq":N,"pong":true,"uptime_s":U,"heap":H}
 * @return 长度（不含 \0）；缓冲区不足返回 0
 */
int bridge_pong_to_json(uint8_t seq, unsigned long long uptime_s,
                        unsigned long heap, char* out, size_t out_size);
```

`components/bridge/src/bridge_codec.c` 追加（放在 `bridge_json_to_control_cmd` 之前）：

```c
bool bridge_json_is_ping(const char* json, size_t len, uint8_t* out_seq)
{
    if (json == NULL || out_seq == NULL)
    {
        return false;
    }
    cJSON* root = cJSON_ParseWithLength(json, len);
    if (root == NULL)
    {
        return false;
    }
    bool is_ping = false;
    const cJSON* cmd = cJSON_GetObjectItemCaseSensitive(root, "cmd");
    if (cmd != NULL && cJSON_IsString(cmd) && strcmp(cmd->valuestring, "ping") == 0)
    {
        *out_seq = (uint8_t)(get_int(root, "seq", 0) & 0xFF);
        is_ping = true;
    }
    cJSON_Delete(root);
    return is_ping;
}

int bridge_pong_to_json(uint8_t seq, unsigned long long uptime_s,
                        unsigned long heap, char* out, size_t out_size)
{
    if (out == NULL || out_size == 0)
    {
        return 0;
    }
    const int n = snprintf(out, out_size,
                           "{\"seq\":%u,\"pong\":true,\"uptime_s\":%llu,\"heap\":%lu}",
                           (unsigned)seq, uptime_s, heap);
    if (n <= 0 || (size_t)n >= out_size)
    {
        return 0;
    }
    return n;
}
```

`components/common/include/cmcar_topics.h` 在上行区追加：

```c
#define CMCAR_TOPIC_PONG      "data/" CMCAR_DEVICE_ID "/pong"
```

然后同步到镜像：

```bash
cp CodeMyCar/esp32_project/components/bridge/include/bridge_codec.h serial_pkt_protocol/include/bridge_codec.h
cp CodeMyCar/esp32_project/components/bridge/src/bridge_codec.c serial_pkt_protocol/src/bridge_codec.c
```

- [ ] **Step 5: 运行测试，确认通过**

```bash
cd /Users/sunyameng/github/legacy/serial_pkt_protocol
cmake --build build
cd build/tests && ctest --output-on-failure
```

Expected: `100% tests passed, 0 tests failed out of 3`。

- [ ] **Step 6: 提交**

```bash
cd /Users/sunyameng/github/legacy
git add CodeMyCar/esp32_project/components/common/include/cmcar_topics.h \
        CodeMyCar/esp32_project/components/bridge/include/bridge_codec.h \
        CodeMyCar/esp32_project/components/bridge/src/bridge_codec.c \
        serial_pkt_protocol/include/bridge_codec.h \
        serial_pkt_protocol/src/bridge_codec.c \
        serial_pkt_protocol/tests/test_bridge_codec.c
git commit -m "feat(codec): add ping detection and pong JSON builder"
```

---

## Task 2: ESP32 拦截 ping 并发布 pong

**Files:**
- Modify: `CodeMyCar/esp32_project/components/bridge/src/bridge.c`（`process_downlink()` 开头）
- Verify: `idf.py build`

**Interfaces:**
- Consumes: `bridge_json_is_ping()`, `bridge_pong_to_json()`（Task 1）、`CMCAR_TOPIC_PONG`、`mqtt_service_publish_timed()`。
- Produces: 无新导出符号。

- [ ] **Step 1: 在 bridge.c 增加 pong 发布函数**

在 `process_downlink()` **之前**（同一文件的静态函数区）新增，避免前置声明：

```c
/** 直接回复 ping（不经过 UART/STM32），用于 MQTT 链路自检 */
static void publish_pong(uint8_t seq)
{
    char json[96];
    const unsigned long long uptime_s =
        (unsigned long long)(esp_timer_get_time() / 1000000LL);
    const int n = bridge_pong_to_json(
        seq, uptime_s, (unsigned long)esp_get_free_heap_size(), json, sizeof(json));
    if (n <= 0)
    {
        ESP_LOGW(TAG, "pong json build failed");
        return;
    }
    const esp_err_t err = mqtt_service_publish_timed(
        CMCAR_TOPIC_PONG, json, (size_t)n, 0, 200);
    if (err != ESP_OK)
    {
        ESP_LOGD(TAG, "pong publish skipped: %s", esp_err_to_name(err));
    }
}
```

并在文件头 includes 增加 `#include "esp_system.h"`（`esp_get_free_heap_size()`）。

- [ ] **Step 2: 在 process_downlink 中拦截**

在 `process_downlink()` 的 `ESP_LOGI(TAG, "CMD downlink len=%zu", payload_len);` 之后、`bridge_json_to_control_cmd()` 之前插入：

```c
    /* ping：MQTT 链路自检，直接回 pong，不进入 UART/STM32 路径 */
    uint8_t ping_seq = 0;
    if (bridge_json_is_ping(payload, payload_len, &ping_seq))
    {
        publish_pong(ping_seq);
        return;
    }
```

- [ ] **Step 3: 编译验证**

```bash
get_idf
cd /Users/sunyameng/github/legacy/CodeMyCar/esp32_project
idf.py fullclean
idf.py build
```

Expected: `Project build complete.`，无 warning/error（尤其无隐式声明警告）。

> `fullclean` 是必须的：当前 `build/` 由 ESP-IDF v6.0.2 生成，引用已删除的路径。

- [ ] **Step 4: 提交**

```bash
cd /Users/sunyameng/github/legacy
git add CodeMyCar/esp32_project/components/bridge/src/bridge.c
git commit -m "feat(esp32): reply to ping over MQTT without STM32"
```

---

## Task 3: pyserver PingService + pong handler

**Files:**
- Create: `CodeMyCar/pyserver/app/services/ping_service.py`
- Modify: `CodeMyCar/pyserver/app/mqtt/topics.py`
- Modify: `CodeMyCar/pyserver/app/mqtt/handlers.py`
- Test: `CodeMyCar/pyserver/tests/test_ping.py`

**Interfaces:**
- Produces:
  - `class PingError(Exception)`
  - `class PingReply(BaseModel)`: `seq: int`, `rtt_ms: float`, `uptime_s: int | None`, `heap: int | None`
  - `class PingRegistry`:
    - `next_seq(device_id: str) -> int`（0–255 回绕，跳过该设备在途 seq；256 个都在途时抛 `PingError`）
    - `register(device_id: str, seq: int) -> asyncio.Future`
    - `pop(device_id: str, seq: int) -> None`
    - `resolve(device_id: str, seq: int, uptime_s: int | None, heap: int | None) -> bool`
  - `class PingService`:
    - `__init__(mqtt, registry: PingRegistry, timeout_s: float = 3.0)`
    - `async ping(device_id: str) -> PingReply`
  - `create_pong_handler(registry: PingRegistry)` → `async def handler(topic: str, payload: bytes) -> None`
- `app/mqtt/topics.py` 增加 `SUB_PONG = "data/+/pong"`。

- [ ] **Step 1: 写失败测试**

新建 `CodeMyCar/pyserver/tests/test_ping.py`：

```python
"""Ping/pong 链路自检测试（无需 broker / 板子）"""

import asyncio
import json

import pytest

from app.mqtt.handlers import create_pong_handler
from app.services.ping_service import PingError, PingRegistry, PingService


class FakeMQTT:
    def __init__(self):
        self.is_connected = True
        self.published: list[tuple[str, object]] = []

    async def publish(self, topic, payload, qos=0):
        self.published.append((topic, payload))


async def test_ping_success_returns_rtt_and_device_info():
    mqtt = FakeMQTT()
    registry = PingRegistry()
    service = PingService(mqtt, registry, timeout_s=1.0)

    task = asyncio.create_task(service.ping("DEV_1"))
    await asyncio.sleep(0.01)

    topic, payload = mqtt.published[0]
    assert topic == "cmd/DEV_1/exec"
    assert payload == {"cmd": "ping", "seq": 0}

    assert registry.resolve("DEV_1", 0, uptime_s=123, heap=80000) is True
    reply = await task

    assert reply.seq == 0
    assert reply.uptime_s == 123
    assert reply.heap == 80000
    assert reply.rtt_ms >= 0


async def test_ping_timeout_raises():
    service = PingService(FakeMQTT(), PingRegistry(), timeout_s=0.05)
    with pytest.raises(PingError):
        await service.ping("DEV_1")


async def test_ping_when_mqtt_disconnected_fails_fast():
    mqtt = FakeMQTT()
    mqtt.is_connected = False
    with pytest.raises(PingError):
        await PingService(mqtt, PingRegistry()).ping("DEV_1")


async def test_concurrent_pings_are_isolated():
    mqtt = FakeMQTT()
    registry = PingRegistry()
    service = PingService(mqtt, registry, timeout_s=1.0)

    t1 = asyncio.create_task(service.ping("DEV_1"))
    await asyncio.sleep(0.01)
    t2 = asyncio.create_task(service.ping("DEV_1"))
    await asyncio.sleep(0.01)

    seq1 = mqtt.published[0][1]["seq"]
    seq2 = mqtt.published[1][1]["seq"]
    assert seq1 != seq2

    registry.resolve("DEV_1", seq2, uptime_s=2, heap=2)
    registry.resolve("DEV_1", seq1, uptime_s=1, heap=1)

    r1, r2 = await asyncio.gather(t1, t2)
    assert (r1.uptime_s, r2.uptime_s) == (1, 2)


async def test_pong_handler_resolves_matching_ping():
    registry = PingRegistry()
    future = registry.register("DEV_1", 7)
    on_pong = create_pong_handler(registry)

    await on_pong(
        "data/DEV_1/pong",
        json.dumps({"seq": 7, "pong": True, "uptime_s": 5, "heap": 100}).encode(),
    )

    assert future.done()
    assert future.result() == {"uptime_s": 5, "heap": 100}


async def test_pong_handler_ignores_bad_payloads():
    registry = PingRegistry()
    on_pong = create_pong_handler(registry)

    await on_pong("data/DEV_1/pong", b"not-json")
    await on_pong("data/DEV_1/pong", json.dumps({"seq": 9, "pong": False}).encode())
    await on_pong("data/DEV_1/pong", json.dumps({"seq": 9}).encode())

    # 未知 seq 不得误唤醒任何等待者
    assert registry.resolve("DEV_1", 9, uptime_s=1, heap=1) is False
```

- [ ] **Step 2: 运行测试，确认失败**

```bash
cd /Users/sunyameng/github/legacy/CodeMyCar/pyserver
make test
```

Expected: `ModuleNotFoundError: No module named 'app.services.ping_service'`（collection error）。

- [ ] **Step 3: 实现 `app/services/ping_service.py`**

```python
"""Ping/pong 链路自检：不依赖 STM32，仅验证 MQTT 上下行"""

import asyncio
import time

from pydantic import BaseModel

from app.mqtt.topics import cmd_topic


class PingError(Exception):
    """ping 失败（未连接 / 超时 / 无法分配 seq）"""


class PingReply(BaseModel):
    seq: int
    rtt_ms: float
    uptime_s: int | None = None
    heap: int | None = None


class PingRegistry:
    """按 (device_id, seq) 关联在途 ping 与 pong"""

    def __init__(self) -> None:
        self._pending: dict[tuple[str, int], asyncio.Future] = {}
        self._seq = 0

    def next_seq(self, device_id: str) -> int:
        for _ in range(256):
            seq = self._seq
            self._seq = (self._seq + 1) & 0xFF
            if (device_id, seq) not in self._pending:
                return seq
        raise PingError("too many in-flight pings")

    def register(self, device_id: str, seq: int) -> asyncio.Future:
        future = asyncio.get_running_loop().create_future()
        self._pending[(device_id, seq)] = future
        return future

    def pop(self, device_id: str, seq: int) -> None:
        self._pending.pop((device_id, seq), None)

    def resolve(
        self, device_id: str, seq: int, uptime_s: int | None, heap: int | None
    ) -> bool:
        future = self._pending.get((device_id, seq))
        if future is None or future.done():
            return False
        future.set_result({"uptime_s": uptime_s, "heap": heap})
        return True


class PingService:
    def __init__(self, mqtt, registry: PingRegistry, timeout_s: float = 3.0) -> None:
        self._mqtt = mqtt
        self._registry = registry
        self._timeout_s = timeout_s

    async def ping(self, device_id: str) -> PingReply:
        if not self._mqtt.is_connected:
            raise PingError("MQTT not connected")

        seq = self._registry.next_seq(device_id)
        future = self._registry.register(device_id, seq)
        started = time.monotonic()
        try:
            await self._mqtt.publish(
                cmd_topic(device_id), {"cmd": "ping", "seq": seq}, qos=0
            )
            reply = await asyncio.wait_for(future, timeout=self._timeout_s)
        except asyncio.TimeoutError:
            raise PingError(
                f"pong timeout after {self._timeout_s:g}s (device {device_id})"
            ) from None
        finally:
            self._registry.pop(device_id, seq)

        return PingReply(
            seq=seq,
            rtt_ms=round((time.monotonic() - started) * 1000, 1),
            uptime_s=reply.get("uptime_s"),
            heap=reply.get("heap"),
        )
```

- [ ] **Step 4: 实现 pong handler 与 topic**

`app/mqtt/topics.py` 末尾追加：

```python
SUB_PONG = "data/+/pong"
```

`app/mqtt/handlers.py` 末尾追加（`create_handlers` 之后）：

```python
def create_pong_handler(registry):
    """ping 回执：按 (device_id, seq) 唤醒等待中的 ping 请求"""

    async def on_pong(topic: str, payload: bytes) -> None:
        device_id = extract_device_id(topic)
        try:
            data = json.loads(payload)
        except Exception:
            logger.debug("Invalid pong from %s", device_id)
            return
        seq = data.get("seq")
        if not isinstance(seq, int) or data.get("pong") is not True:
            logger.debug("Ignoring malformed pong from %s", device_id)
            return
        registry.resolve(device_id, seq & 0xFF, data.get("uptime_s"), data.get("heap"))

    return on_pong
```

- [ ] **Step 5: 运行测试，确认通过**

```bash
cd /Users/sunyameng/github/legacy/CodeMyCar/pyserver
make test
```

Expected: 全部通过（原有测试全部保持通过 + 新增 7 项）。

- [ ] **Step 6: 提交**

```bash
cd /Users/sunyameng/github/legacy
git add CodeMyCar/pyserver/app/services/ping_service.py \
        CodeMyCar/pyserver/app/mqtt/topics.py \
        CodeMyCar/pyserver/app/mqtt/handlers.py \
        CodeMyCar/pyserver/tests/test_ping.py
git commit -m "feat(pyserver): add ping registry/service and pong handler"
```

---

## Task 4: pyserver API 路由与装配

**Files:**
- Modify: `CodeMyCar/pyserver/app/api/deps.py`
- Modify: `CodeMyCar/pyserver/app/api/routes/devices.py`
- Modify: `CodeMyCar/pyserver/app/main.py`
- Test: `CodeMyCar/pyserver/tests/test_ping.py`（追加 API 用例）

**Interfaces:**
- Consumes: `PingRegistry`, `PingService`, `PingError`, `create_pong_handler`（Task 3）；`SUB_PONG`。
- Produces:
  - `get_ping_service(request: Request) -> PingService`（每次请求用 `app.state.mqtt_client` 构造，便于测试替换）
  - `POST {api_prefix}/devices/{device_id}/ping` → `CommonResponse`
    - 成功：`{"success":true,"message":"pong","data":{"seq":N,"rtt_ms":..,"uptime_s":..,"heap":..}}`
    - 失败：`{"success":false,"message":"<原因>","data":null}`

- [ ] **Step 1: 写失败测试（追加到 `tests/test_ping.py`）**

```python
from app.api.deps import get_ping_service
from app.main import app
from app.services.ping_service import PingReply


class _OkPingService:
    async def ping(self, device_id):
        return PingReply(seq=3, rtt_ms=12.5, uptime_s=10, heap=5000)


class _TimeoutPingService:
    async def ping(self, device_id):
        raise PingError("pong timeout after 3s")


def test_ping_endpoint_success(client):
    app.dependency_overrides[get_ping_service] = lambda: _OkPingService()
    try:
        resp = client.post("/api/v1/devices/DEV_1/ping")
    finally:
        app.dependency_overrides.clear()

    assert resp.status_code == 200
    body = resp.json()
    assert body["success"] is True
    assert body["data"]["rtt_ms"] == 12.5


def test_ping_endpoint_reports_failure(client):
    app.dependency_overrides[get_ping_service] = lambda: _TimeoutPingService()
    try:
        resp = client.post("/api/v1/devices/DEV_1/ping")
    finally:
        app.dependency_overrides.clear()

    body = resp.json()
    assert body["success"] is False
    assert "timeout" in body["message"]
```

- [ ] **Step 2: 运行测试，确认失败**

```bash
cd /Users/sunyameng/github/legacy/CodeMyCar/pyserver
make test
```

Expected: `ImportError: cannot import name 'get_ping_service'`。

- [ ] **Step 3: 实现 deps、路由与 main 装配**

`app/api/deps.py` 追加：

```python
def get_ping_service(request: Request) -> PingService:
    return PingService(get_mqtt_client(request), request.app.state.ping_registry)
```

并 import `PingService`。

`app/api/routes/devices.py`（顶部已有 `from fastapi import APIRouter, Depends`，只需补充下面两行 import）追加：

```python
from app.api.deps import get_ping_service
from app.services.ping_service import PingError, PingService


@router.post("/devices/{device_id}/ping", response_model=CommonResponse)
async def ping_device(
    device_id: str,
    service: PingService = Depends(get_ping_service),
):
    """手动 MQTT 链路自检：ESP32 直接回 pong（不依赖 STM32）"""
    try:
        reply = await service.ping(device_id)
    except PingError as exc:
        return CommonResponse(success=False, message=str(exc))
    return CommonResponse(success=True, message="pong", data=reply.model_dump())
```

`app/main.py`：

1. import 增加：

```python
from app.mqtt.handlers import create_handlers, create_pong_handler
from app.mqtt.topics import SUB_ACK, SUB_HEARTBEAT, SUB_LOG, SUB_PONG, SUB_SENSOR
from app.services.ping_service import PingRegistry
```

2. 在 `app.state.mqtt_client = mqtt_client` 之后：

```python
    # ping/pong：注册表放在 app.state，handler 与每次请求的 PingService 共享
    app.state.ping_registry = PingRegistry()
```

3. 在 ACK handler 注册之后：

```python
    mqtt_client.register_handler(SUB_PONG, create_pong_handler(app.state.ping_registry))
```

- [ ] **Step 4: 运行测试，确认通过**

```bash
cd /Users/sunyameng/github/legacy/CodeMyCar/pyserver
make test
```

Expected: 全部通过（原有测试全部保持通过 + Task 3 的 7 项 + 新增 2 项）。

- [ ] **Step 5: 提交**

```bash
cd /Users/sunyameng/github/legacy
git add CodeMyCar/pyserver/app/api/deps.py \
        CodeMyCar/pyserver/app/api/routes/devices.py \
        CodeMyCar/pyserver/app/main.py \
        CodeMyCar/pyserver/tests/test_ping.py
git commit -m "feat(pyserver): expose POST /devices/{id}/ping"
```

---

## Task 5: 手动入口（Makefile）与文档

**Files:**
- Modify: `CodeMyCar/pyserver/Makefile`
- Modify: `CodeMyCar/esp32_project/docs/mqtt-topics.md`
- Modify: `CodeMyCar/pyserver/README.md`
- Modify: `CodeMyCar/esp32_project/CLAUDE.md`（topic 列表补充 pong）

**Interfaces:**
- Produces: `make ping [DEVICE=DEV_1]` → 调用 `POST /api/v1/devices/{DEVICE}/ping` 并格式化输出。

- [ ] **Step 1: 增加 Makefile 目标**

在 `.PHONY` 行加入 `ping`，并在 `mqtt-pub` 之后追加：

```make
ping:  ## 手动链路自检：make ping [DEVICE=DEV_1]
	@curl -s -X POST "http://127.0.0.1:8000/api/v1/devices/$(or $(DEVICE),DEV_1)/ping" \
		| python3 -m json.tool
```

- [ ] **Step 2: 校验 Makefile 语法**

```bash
cd /Users/sunyameng/github/legacy/CodeMyCar/pyserver
make -n ping
```

Expected: 打印出 `curl -s -X POST http://127.0.0.1:8000/api/v1/devices/DEV_1/ping | python3 -m json.tool`。

- [ ] **Step 3: 更新文档**

`CodeMyCar/esp32_project/docs/mqtt-topics.md`：

- 下行表补一行：`cmd/{device_id}/exec` 支持 `{"cmd":"ping","seq":N}`（ESP32 直接回 pong，不转发 STM32）。
- 上行表补一行：`data/{device_id}/pong` | ping 回执 | `{"seq":N,"pong":true,"uptime_s":U,"heap":H}`。
- pyserver 订阅列表补 `data/+/pong`。

`CodeMyCar/pyserver/README.md`：在 API 说明处补充手动自检用法：

```bash
make ping                 # 默认 DEV_1
make ping DEVICE=DEV_1
```

`CodeMyCar/esp32_project/CLAUDE.md` 的 topic 列表补 `CMCAR_TOPIC_PONG = "data/" DEVICE_ID "/pong"`。

- [ ] **Step 4: 提交**

```bash
cd /Users/sunyameng/github/legacy
git add CodeMyCar/pyserver/Makefile CodeMyCar/pyserver/README.md \
        CodeMyCar/esp32_project/docs/mqtt-topics.md CodeMyCar/esp32_project/CLAUDE.md
git commit -m "docs: document MQTT ping/pong self-check and make ping target"
```

---

## 手工验收（开发完成后）

1. 启动本机 broker + pyserver：
   ```bash
   cd /Users/sunyameng/github/legacy/CodeMyCar/pyserver && make dev
   ```
2. 烧录并上电 ESP32（`idf.py -p /dev/cu.usbmodem101 flash monitor`）。
3. 另一个终端：
   ```bash
   cd /Users/sunyameng/github/legacy/CodeMyCar/pyserver
   make ping
   ```
   期望：`{"success": true, "message": "pong", "data": {"seq": 0, "rtt_ms": ..., "uptime_s": ..., "heap": ...}}`
4. 拔掉 STM32（或从不上电）依然应成功——证明不依赖 MCU。
5. 停掉 ESP32 电源后再 `make ping`：3 秒后返回 `success: false`，`message` 含 `pong timeout`。
