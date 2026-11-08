"""Zero wheel RPM only; small steering +/-0.5/m. No distance-driving command."""
import json,time,gzip,threading,urllib.request
from pathlib import Path
import paho.mqtt.client as mqtt
b=Path('stm32_project/Docs/validation/path-tracking-2026-11-08');name='mag-servo-only-01';events=[];summary={'stages':[]};ready=threading.Event();acks={};expired=threading.Event();timer=None
assert not (b/(name+'.jsonl.gz')).exists()
def req(path,data=None,post=False):
 q=urllib.request.Request('http://127.0.0.1:8000/api/v1'+path,data=json.dumps(data).encode() if data is not None else None,headers={'Content-Type':'application/json'},method='POST' if post else 'GET')
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
 if m.topic.endswith('/ack'):acks[d['seq']]=d
c=mqtt.Client(mqtt.CallbackAPIVersion.VERSION2,client_id='cmcar-servo-only-magnetic-check');c.on_connect=connected;c.on_message=message
try:
 summary['pre_stop']=stop();c.connect('127.0.0.1',1883);c.loop_start();assert ready.wait(3)
 time.sleep(2);initial=req('/agent/devices/DEV_1/state');summary['before']=initial;assert initial['online'] and initial['stm32_connected'] and abs(initial['velocity']['linear_cm_s'])<.1
 origin=initial['pose']['odometer_cm'];seq=64;timer=threading.Timer(16,deadline);timer.start()
 for k in [0,-.5,0,.5,0]:
  stage={'curvature_1pm':k,'start_t':time.time(),'seq_start':seq};summary['stages'].append(stage);started=time.monotonic()
  while time.monotonic()-started<2.4:
   assert not expired.is_set(),'watchdog'
   st=req('/agent/devices/DEV_1/state');assert st['online'] and st['stm32_connected'] and not st['motion']['stale']
   assert abs(st['velocity']['linear_cm_s'])<2 and abs(st['pose']['odometer_cm']-origin)<.5,'unexpected wheel motion'
   assert seq<230
   assert all(v.get('ack') and v.get('status')==0 for n,v in acks.items() if 64<=n<seq),'command rejected'
   d={'seq':seq,'cmd':'motion','motion':{'mode':1,'params':{'left_rpm':0,'right_rpm':0,'curvature_1pm':k}}};assert req('/devices/DEV_1/commands',d,True).get('success');seq+=1;time.sleep(.1)
  stage['end_t']=time.time();stage['seq_end']=seq-1;print('STAGE',stage,flush=True)
except Exception as e:
 summary['error']=repr(e);print('ERROR',repr(e),flush=True)
finally:
 summary['stop']=stop()
 if timer:timer.cancel()
 time.sleep(.4);summary['after']=req('/agent/devices/DEV_1/state');c.disconnect();c.loop_stop()
 (b/(name+'.jsonl.gz')).write_bytes(gzip.compress(''.join(json.dumps(e)+'\n' for e in events).encode(),mtime=0));(b/(name+'-summary.json')).write_text(json.dumps(summary,indent=2)+'\n')
