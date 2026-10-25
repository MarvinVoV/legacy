#!/usr/bin/env bash
# dev.sh — 一键启动本地开发环境（mosquitto + uvicorn --reload）
#
# 用法：
#   ./scripts/dev.sh                      # 本机 mosquitto + uvicorn 热重载
#   MQTT_HOST=192.168.1.10 ./scripts/dev.sh  # 指向远端 broker，只起 python 服务
#
# 退出（Ctrl-C）会自动清理本脚本拉起的 mosquitto 进程。
# 远端模式下不清理本地 mosquitto（没起）。

set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

# ---- 公共：准备 .env ----
if [[ ! -f .env ]]; then
  echo "→ 从 .env.example 生成 .env"
  cp .env.example .env
fi

# 决定 MQTT host：显式 MQTT_HOST 优先，否则默认 localhost（本地 broker）
MQTT_HOST="${MQTT_HOST:-localhost}"
if grep -q '^CMCAR_MQTT_HOST=' .env; then
  sed -i.bak "s|^CMCAR_MQTT_HOST=.*|CMCAR_MQTT_HOST=$MQTT_HOST|" .env
else
  echo "CMCAR_MQTT_HOST=$MQTT_HOST" >> .env
fi
rm -f .env.bak

# ---- 远端 broker：跳过本地 mosquitto ----
LOCAL_BROKER=1
if [[ "$MQTT_HOST" != "localhost" && "$MQTT_HOST" != "127.0.0.1" ]]; then
  echo "→ MQTT_HOST=$MQTT_HOST，跳过本地 mosquitto"
  LOCAL_BROKER=0
fi

MQTT_PID=""
cleanup() {
  if [[ -n "$MQTT_PID" ]]; then
    echo
    echo "→ 停止 mosquitto (pid $MQTT_PID)"
    kill "$MQTT_PID" 2>/dev/null || true
    wait "$MQTT_PID" 2>/dev/null || true
  fi
}
trap cleanup EXIT INT TERM

# ---- 本地模式：拉起 mosquitto ----
if [[ "$LOCAL_BROKER" == "1" ]]; then
  MOSQ_BIN=""
  for cand in \
    "/opt/homebrew/opt/mosquitto/sbin/mosquitto" \
    "/usr/local/sbin/mosquitto" \
    "/usr/sbin/mosquitto" \
    "$(command -v mosquitto 2>/dev/null || true)"; do
    if [[ -x "$cand" ]]; then MOSQ_BIN="$cand"; break; fi
  done
  if [[ -z "$MOSQ_BIN" ]]; then
    echo "✗ 找不到 mosquitto。安装：brew install mosquitto" >&2
    exit 1
  fi
  echo "→ 启动 mosquitto ($MOSQ_BIN) — 1883 端口，匿名"
  "$MOSQ_BIN" -c "$ROOT/mosquitto.dev.conf" &
  MQTT_PID=$!
  for _ in 1 2 3 4 5 6 7 8 9 10; do
    if nc -z localhost 1883 2>/dev/null; then break; fi
    sleep 0.3
  done
fi

# ---- python 服务（venv + uvicorn --reload） ----
if [[ ! -d .venv ]]; then
  echo "→ 创建 .venv 并安装依赖"
  python3 -m venv .venv
  .venv/bin/pip install -q --upgrade pip
  .venv/bin/pip install -q -e '.[dev]'
fi

echo
echo "  API:    http://localhost:8000"
echo "  Docs:   http://localhost:8000/docs"
echo "  MQTT:   $MQTT_HOST:1883"
echo "  退出：Ctrl-C"
echo

source .venv/bin/activate
exec uvicorn app.main:app --reload --host 0.0.0.0 --port "${CMCAR_WEB_PORT:-8000}"
