# 服务端测试入口

完整清单与复用说明见 [控制系统测试指南](../../stm32_project/Docs/Control-Test-Guide-For-Agents.md) 第5节；端到端设备验收见第6节。

```bash
cd "$PROJECT_ROOT/pyserver"
.venv/bin/python -m pytest -q
.venv/bin/python -m pytest --collect-only -q
.venv/bin/python -m pytest -q tests/test_stop_session_sync.py
.venv/bin/python -m pytest -q tests/test_stop_session_sync.py::test_same_seq_old_session_never_confirms_current_stop
```

截至2026-11-08收集85项，以当前collect结果为准。依赖见pyproject.toml的dev分组。

[conftest.py](conftest.py) 禁用真实MQTT连接；client/app与FakeMQTT用于离线API测试。独立工程复制测试时也需保留隔离逻辑。需要模拟会话/ACK/状态到达顺序时参考 [test_stop_session_sync.py](test_stop_session_sync.py)，通过正式handlers驱动服务，勿直接把pending标为成功。

以上命令不启动broker、不烧录、不驱动车辆。真实HIL脚本位于stm32_project/tests/hil，执行前需满足指南的设备条件。
