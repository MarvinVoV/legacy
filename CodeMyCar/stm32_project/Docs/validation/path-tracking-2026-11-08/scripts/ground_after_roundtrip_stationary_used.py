import json,time,gzip,threading,urllib.request,re,statistics,math
from pathlib import Path
import paho.mqtt.client as mqtt
base=Path('stm32_project/Docs/validation/path-tracking-2026-11-08');name='ground-after-roundtrip-stationary-30s-01';events=[];summary={};ready=threading.Event()
assert not (base/(name+'.jsonl.gz')).exists()
def request(path,post=False):
 q=urllib.request.Request('http://127.0.0.1:8000/api/v1'+path,method='POST' if post else 'GET')
 with urllib.request.urlopen(q,timeout=6) as f:return json.load(f)
def stop():
 s=request('/agent/devices/DEV_1/stop',True);assert all(s.get(k) for k in ('accepted','confirmed','stopped')),s;return s
def connected(c,u,f,r,p):c.subscribe('data/DEV_1/#');ready.set()
def message(c,u,m):
 try:events.append({'t':time.time(),'topic':m.topic,'data':json.loads(m.payload)})
 except ValueError:pass
c=mqtt.Client(mqtt.CallbackAPIVersion.VERSION2,client_id='cmcar-stationary-audit');c.on_connect=connected;c.on_message=message
try:
 summary['pre_stop']=stop();print('STOP',summary['pre_stop'],flush=True)
 c.connect('127.0.0.1',1883);c.loop_start();assert ready.wait(3)
 summary['before']=request('/agent/devices/DEV_1/state');summary['start_t']=time.time();print('COLLECTING_30S_NO_MOTION',flush=True)
 for i in range(30):time.sleep(1)
 summary['end_t']=time.time();summary['after']=request('/agent/devices/DEV_1/state')
finally:
 summary['stop']=stop();c.disconnect();c.loop_stop()
 (base/(name+'.jsonl.gz')).write_bytes(gzip.compress(''.join(json.dumps(e)+'\n' for e in events).encode(),mtime=0))
 (base/(name+'-summary.json')).write_text(json.dumps(summary,indent=2))
print('FINAL_STOP',summary['stop'],flush=True)
