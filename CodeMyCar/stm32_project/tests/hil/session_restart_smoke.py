"""Explicit ESP32 reboot-only session test; never sends a movement command."""
import argparse
import json
import subprocess
import time
import urllib.request
import urllib.error

from control_smoke import Capture

parser=argparse.ArgumentParser()
parser.add_argument('--port',required=True)
parser.add_argument('--esptool-python',required=True)
parser.add_argument('--cycles',type=int,default=3)
parser.add_argument('--chip', choices=('esp32c6','esp32s3'), default='esp32c6')
args=parser.parse_args()
capture=Capture('DEV_1')
url='http://127.0.0.1:8000/api/v1/agent/devices/DEV_1/stop'
http = urllib.request.build_opener(urllib.request.ProxyHandler({}))
def stop():
    with http.open(urllib.request.Request(url,method='POST'),timeout=8) as r:
        result=json.load(r)
    assert result.get('confirmed'),result
    return result
try:
    stop()
    previous=capture.wait('motion',lambda x:'session_id' in x,time.monotonic()-1,3)['session_id']
    for i in range(args.cycles):
        subprocess.run([args.esptool_python,'-m','esptool','--chip',args.chip,
            '--port',args.port,'--before','default-reset','--after','hard-reset','read-mac'],
            check=True,stdout=subprocess.PIPE,stderr=subprocess.PIPE,timeout=15)
        start=time.monotonic()
        heartbeat=capture.wait('heartbeat',lambda x:x.get('session_id')!=previous,start,15)
        announced=time.monotonic()-start
        assert heartbeat.get('uptime_s',99)<15,heartbeat
        try:
            result=stop()
        except urllib.error.HTTPError as exc:
            print('STOP_ERROR',exc.read().decode(),flush=True)
            print('HB',json.dumps(heartbeat),flush=True)
            for stamp,kind,data in capture.events[-100:]:
                if kind in ('ack','motion','heartbeat','status'):
                    print('RECENT',kind,json.dumps(data),flush=True)
            raise
        print(json.dumps({'reboot':i+1,'session':heartbeat['session_id'],
            'first_heartbeat_uptime_s':heartbeat['uptime_s'],
            'wait_after_reset_ms':round(announced*1000,1),'stop':result}),flush=True)
        previous=heartbeat['session_id']
    print(f'REBOOT_SESSION_PASS {args.cycles}/{args.cycles}',flush=True)
finally:
    try:stop()
    finally:
        capture.client.disconnect()
        capture.client.loop_stop()
