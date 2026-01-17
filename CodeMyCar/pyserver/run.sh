#!/bin/bash
# run.sh

# 配置参数
MQTT_PORT=1883
APP_PORT=8000
MOSQUITTO_CONF="/opt/homebrew/etc/mosquitto/mosquitto.conf"
CONFIG_DIR="${HOME}/.pyserver"
# 创建运行目录
mkdir -p "${CONFIG_DIR}"

start_mosquitto() {
    if ! lsof -i :${MQTT_PORT} | grep LISTEN; then
        echo "Starting Mosquitto..."
        mosquitto -c ${MOSQUITTO_CONF} >> mosquitto.log 2>&1 &
        echo $! > "${CONFIG_DIR}"/mosquitto.pid
        echo "Mosquitto started (PID: $(cat "${CONFIG_DIR}"/mosquitto.pid))"
    else
        echo "Mosquitto already running on port ${MQTT_PORT}"
    fi
}

stop_mosquitto() {
    if [ -f "${CONFIG_DIR}"/mosquitto.pid ]; then
        MOSQUITTO_PID=$(cat "${CONFIG_DIR}"/mosquitto.pid)
        kill -9 "${MOSQUITTO_PID}" 2>/dev/null && echo "Stopped Mosquitto (PID: ${MOSQUITTO_PID})" || echo "Mosquitto process not found"
        rm "${CONFIG_DIR}"/mosquitto.pid
   fi
}

start_app() {
    if ! lsof -i :${APP_PORT} | grep LISTEN; then
        echo "Starting FastAPI service..."
        uvicorn app.main:app --host 0.0.0.0 --port ${APP_PORT} --reload >> app.log 2>&1 &
        echo $! > "${CONFIG_DIR}"/app.pid
        echo "API started (PID: $(cat "${CONFIG_DIR}"/app.pid))"
    else
        echo "FastAPI already running on port ${APP_PORT}"
    fi
}

stop_app() {
    if [ -f "${CONFIG_DIR}"/app.pid ]; then
        APP_PID=$(cat "${CONFIG_DIR}"/app.pid)
        kill -9 "${APP_PID}" 2>/dev/null && echo "Stopped API (PID: ${APP_PID})" || echo "API process not found"
        rm "${CONFIG_DIR}"/app.pid
    fi
}

# 1. 服务启动函数
start_services() {
    # 启动Mosquitto
    start_mosquitto
    # 启动FastAPI
    start_app
     # 持续运行提示
    echo "Services are running. Use './run.sh stop' to terminate"
    wait
}


# 2. 服务停止函数
stop_services() {
    echo "Stopping services..."
    # 停止Mosquitto
    stop_mosquitto

    # 停止FastAPI
    stop_app
}

# 3. 帮助信息
show_help() {
    echo "Usage: $0 {start|stop|restart}"
    echo "  start   - 启动所有服务"
    echo "  stop    - 停止所有服务"
    echo "  restart - 重启所有服务"
    echo "  start_mqtt   - 启动Mosquitto服务"
    echo "  stop_mqtt    - 停止Mosquitto服务"
}

# 主流程
case "$1" in
    start)
        start_services
        ;;
    stop)
        stop_services
        ;;
    restart)
        stop_services
        sleep 2
        start_services
        ;;
    start_mqtt)
        start_mosquitto
        ;;
    stop_mqtt)
        stop_mosquitto
        ;;
    *)
        show_help
        exit 1
esac