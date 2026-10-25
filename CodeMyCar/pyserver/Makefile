# CodeMyCar Server — 开发命令速查
# 用法：make <target>   （make dev 首次会自动建 .venv 并装依赖）

ROOT := $(shell pwd)
PY   := .venv/bin/python

.DEFAULT_GOAL := help

.PHONY: help dev venv test test-watch mqtt-sub mqtt-pub ping shell clean

help:  ## 显示所有命令
	@awk 'BEGIN{FS=":.*##"; printf "用法:\n  make \033[36m<target>\033[0m\n\n"} \
	/^[a-zA-Z_-]+:.*##/ { printf "  \033[36m%-14s\033[0m %s\n", $$1, $$2 }' $(MAKEFILE_LIST)

dev:  ## 一键：mosquitto + uvicorn --reload（Ctrl-C 退出，自动清理）
	./scripts/dev.sh

venv:  ## 创建/更新 .venv 并安装依赖（dev/test 自动调用）
	@test -x .venv/bin/python || { python3 -m venv .venv && .venv/bin/pip install -q --upgrade pip; }
	@.venv/bin/pip install -q -e '.[dev]'
	@touch .venv

test: venv  ## 跑单元测试（pytest，无需 broker/板子）
	$(PY) -m pytest -q

test-watch:  ## 改动自动重跑测试（需 entr：brew install entr）
	@command -v entr >/dev/null || { echo "请先 brew install entr"; exit 1; }
	@find app tests | entr -r $(PY) -m pytest -q

mqtt-sub:  ## 订阅全部 topic（本地 broker）
	mosquitto_sub -h localhost -p 1883 -t 'data/#' -t 'cmd/#' -v

mqtt-pub:  ## 下发一条测试命令：make mqtt-pub MODE=2
	@test -n "$(MODE)" || { echo "用法: make mqtt-pub MODE=2"; exit 1; }
	@mosquitto_pub -h localhost -p 1883 -t 'cmd/DEV_1/exec' \
		-m '{"seq":1,"cmd":"motion","motion":{"mode":$(MODE),"duration":1000,"params":{"linear_vel":500,"angular_vel":100}}}'

ping:  ## 手动链路自检：make ping [DEVICE=DEV_1]
	@curl -s -X POST "http://127.0.0.1:8000/api/v1/devices/$(or $(DEVICE),DEV_1)/ping" \
		| python3 -m json.tool

shell:  ## 进入 venv 的 python REPL（已加载 app）
	$(PY) -i -c 'import app.main; from app.config import get_settings; print(get_settings())'

clean:  ## 删除 .venv 与 __pycache__
	rm -rf .venv .pytest_cache
	find . -type d -name __pycache__ -exec rm -rf {} + 2>/dev/null || true
