#!/usr/bin/env python3
"""Observe display telemetry; --cycle-moods explicitly switches five expressions.
Run from pyserver using its venv and PYTHONPATH=.; never sends motion commands.
"""
import argparse
import json
import re
import threading
import time
from pathlib import Path
import httpx
import paho.mqtt.client as mqtt
from app.config import get_settings


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--seconds', type=int, default=30)
    parser.add_argument('--cycle-moods', action='store_true')
    args = parser.parse_args()
    if not 10 <= args.seconds <= 600:
        parser.error('--seconds must be 10..600')
    settings = get_settings()
    events, ready, lock = [], threading.Event(), threading.Lock()
    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=f'display-check-{time.time_ns()}')
    if settings.mqtt_username:
        client.username_pw_set(settings.mqtt_username, settings.mqtt_password)

    def connected(c, _userdata, _flags, reason, _props):
        if reason == 0:
            c.subscribe([(f'data/DEV_1/{kind}', 0) for kind in ('log', 'motion', 'heartbeat', 'sensor', 'status')])

    def message(_client, _userdata, msg):
        try:
            data = json.loads(msg.payload)
        except (ValueError, UnicodeError):
            return
        kind = msg.topic.rsplit('/', 1)[-1]
        if kind == 'log' and not any(word in data.get('message', '') for word in
                                   ('DISP ', 'PERF ', 'RTOS ', 'STACK ', 'IMU ')):
            return
        with lock:
            events.append({'time': time.time(), 'kind': kind, 'data': data})
        if kind == 'log' and 'DISP ' in data.get('message', ''):
            print(data['message'], flush=True)

    client.on_connect = connected
    client.on_subscribe = lambda *_: ready.set()
    client.on_message = message
    report = {'cycle_moods': args.cycle_moods, 'passed': False}
    headers = {'X-API-Key': settings.api_key} if settings.api_key else {}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with httpx.Client(base_url='http://127.0.0.1:8000/api/v1', headers=headers,
                      timeout=5, trust_env=False) as http:
        def get(path):
            response = http.get(path)
            response.raise_for_status()
            return response.json()

        def hint(payload):
            response = http.post('/devices/DEV_1/lighting', json=payload)
            response.raise_for_status()
            if not response.json().get('success'):
                raise RuntimeError(response.json())

        def check_state(state):
            m = state['motion']
            if not (state['online'] and state['stm32_connected'] and not m['stale'] and
                    not m['fault'] and m['mode'] == 0 and abs(state['velocity']['linear_cm_s']) < .1):
                raise RuntimeError(f'Vehicle must remain stopped and healthy: {state}')

        try:
            report['initial_state'] = get('/agent/devices/DEV_1/state')
            check_state(report['initial_state'])
            client.connect(settings.mqtt_host, settings.mqtt_port)
            client.loop_start()
            if not ready.wait(5):
                raise RuntimeError('MQTT subscription timeout')
            if args.cycle_moods:
                for emotion in ('calm', 'happy', 'curious', 'comforting', 'sad'):
                    print(f'Expression: {emotion} (6 seconds)', flush=True)
                    hint({'emotion': emotion, 'duration_ms': 8000})
                    time.sleep(6)
                    check_state(get('/agent/devices/DEV_1/state'))
            else:
                time.sleep(args.seconds)
            report['final_status'] = get('/devices/DEV_1/status')
            report['final_state'] = get('/agent/devices/DEV_1/state')
            check_state(report['final_state'])
            with lock:
                samples = [e['data'] for e in events if e['kind'] == 'motion']
                logs = [e['data']['message'] for e in events if e['kind'] == 'log'
                        and 'DISP fps10=' in e['data'].get('message', '')]
            if not samples or not logs:
                raise RuntimeError('Missing motion telemetry or new display performance logs')
            for sample in samples:
                if (sample.get('fault') or sample.get('error') or
                    abs(sample.get('left_mmps', 999)) > 2 or abs(sample.get('right_mmps', 999)) > 2):
                    raise RuntimeError(f'Unexpected wheel telemetry: {sample}')
            if any('errors=0 ' not in line for line in logs):
                raise RuntimeError(f'Display transfer errors: {logs}')
            if args.cycle_moods:
                observed = {int(match[1]) for line in logs
                            if (match := re.search(r'mood=(\d+)', line))}
                if not set(range(5)).issubset(observed):
                    raise RuntimeError(f'Missing firmware mood confirmations: {observed}')
            report['display_logs'], report['passed'] = logs, True
        except Exception as exc:
            report['error'] = str(exc)
            raise
        finally:
            try:
                if args.cycle_moods:
                    hint({'clear': True})
            finally:
                client.disconnect()
                client.loop_stop()
                with lock:
                    report['events'] = list(events)
                args.output.write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n')
    print(f'Software checks passed. Physical screen observation is still required. {args.output}')


if __name__ == '__main__':
    main()
