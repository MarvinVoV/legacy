## 创建 Python 虚拟环境

```bash
python3 -m venv .venv
```

## 激活 Python 虚拟环境

```bash
source .venv/bin/activate
```

## 安装依赖包

```bash
pip install -r requirements.txt
```

## 运行程序

```bash
python3 uart_serial_test.py
```

## 退出虚拟环境

```bash
deactivate
```

```shell
/opt/homebrew/opt/mosquitto/sbin/mosquitto -c /opt/homebrew/etc/mosquitto/mosquitto.conf
```

## mqtt topic

[前缀]/[设备类型]/[设备ID]/[数据类型]

1. 设备命令下发（设备订阅）
   cmd/[device_type]/[device_id]/exec
2. 设备数据上报（服务端订阅）
   data/[device_type]/[device_id]/sensor
   data/+/+/heartbeat
