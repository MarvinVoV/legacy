import json,time,gzip,threading,urllib.request,sys
from pathlib import Path
import paho.mqtt.client as mqtt
if sys.argv[1:]!=['--suspended-ready']:raise SystemExit('requires confirmed suspended fixed chassis')
b=Path('stm32_project/Docs/validation/path-tracking-2026-11-08');name='fixed-chassis-left-wheel-probe-02';events=[];summary={};ready=threading.Event();bad=threading.Event();expired=threading.Event();timer=None
assert not (b/(name+'.jsonl.gz')).exists()
def req(path,d=None,post=False):
 q=urllib.request.Request('http://127.0.0.1:8000/api/v1'+path,data=json.dumps(d).encode() if d is not None else None,headers={'Content-Type':'application/json'},method='POST' if post else 'GET')
 with urllib.request.urlopen(q,timeout=4) as f:return json.load(f)
def stop():
 d=req('/agent/devices/DEV_1/stop',post=True);print('STOP',d,flush=True);assert all(d.get(k) for k in ('accepted','confirmed','stopped'));return d
def deadline():
 expired.set()
 try:stop()
 except Exception as e:print('WATCHDOG_STOP_ERROR',repr(e),flush=True)
def connected(c,u,f,r,p):c.subscribe('data/DEV_1/#');ready.set()
def message(c,u,m):
 try:d=json.loads(m.payload)
 except ValueError:return
 events.append({'t':time.time(),'topic':m.topic,'data':d})
 if m.topic.endswith('/motion') and (d.get('fault') or abs(d.get('left_mmps',0))>180 or abs(d.get('right_mmps',0))>30):bad.set()
 if m.topic.endswith('/ack') and d.get('seq',0)>=64 and d.get('status')!=0:bad.set()
c=mqtt.Client(mqtt.CallbackAPIVersion.VERSION2,client_id='cmcar-fixed-chassis-wheel-probe');c.on_connect=connected;c.on_message=message
try:
 summary['pre_stop']=stop();c.connect('127.0.0.1',1883);c.loop_start();assert ready.wait(3);time.sleep(3)
 st=req('/agent/devices/DEV_1/state');assert st['online'] and st['stm32_connected'] and abs(st['velocity']['linear_cm_s'])<.1;summary['before']=st
 timer=threading.Timer(10,deadline);timer.start();summary['start_t']=time.time();start=time.monotonic();seq=64
 while time.monotonic()-start<8:
  assert not bad.is_set() and not expired.is_set(),'rejected/unexpected wheel speed/fault/timeout'
  st=req('/agent/devices/DEV_1/state');assert st['online'] and st['stm32_connected'] and not st['motion']['stale']
  assert seq<150
  p={'seq':seq,'cmd':'motion','motion':{'mode':1,'params':{'left_rpm':30,'right_rpm':0,'curvature_1pm':0}}};assert req('/devices/DEV_1/commands',p,True)['success'];seq+=1;time.sleep(.1)
 summary['end_t']=time.time()
except Exception as e:summary['error']=repr(e);print('ERROR',repr(e),flush=True)
finally:
 summary['stop']=stop()
 if timer:timer.cancel()
 time.sleep(.5);summary['after']=req('/agent/devices/DEV_1/state');c.disconnect();c.loop_stop()
 (b/(name+'.jsonl.gz')).write_bytes(gzip.compress(''.join(json.dumps(e)+'\n' for e in events).encode(),mtime=0));(b/(name+'-summary.json')).write_text(json.dumps(summary,indent=2)+'\n');print('AFTER',json.dumps(summary['after']),flush=True)
