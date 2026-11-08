"""Manual posture calibration observer. STOP only; no motion or calibration-setting command."""
import json,time,gzip,threading,urllib.request,os
from pathlib import Path
import paho.mqtt.client as mqtt
b=Path('stm32_project/Docs/validation/path-tracking-2026-11-08');name='mag-manual-calibration-01';signal=Path('/private/tmp/cmcar-mag-calibration-done-1108');events=[];summary={};ready=threading.Event();live={};unsafe=threading.Event()
assert not signal.exists() and not (b/(name+'.jsonl.gz')).exists()
def stop():
 q=urllib.request.Request('http://127.0.0.1:8000/api/v1/agent/devices/DEV_1/stop',method='POST')
 with urllib.request.urlopen(q,timeout=6) as f:s=json.load(f)
 assert all(s.get(k) for k in ('accepted','confirmed','stopped'));return s
def connected(c,u,f,r,p):c.subscribe('data/DEV_1/#');ready.set()
def message(c,u,m):
 try:d=json.loads(m.payload)
 except ValueError:return
 events.append({'t':time.time(),'topic':m.topic,'data':d})
 msg=d.get('message','')
 if msg.startswith('IMU_MAG '):live['mag']=msg
 if msg.startswith('IMU_FIELD '):live['field']=msg
 if msg.startswith('IMU_TIME '):live['timing']=msg
 if m.topic.endswith('/motion'):
  live['motion']=d
  if d.get('mode')!=0:unsafe.set()
c=mqtt.Client(mqtt.CallbackAPIVersion.VERSION2,client_id='cmcar-manual-mag-capture');c.on_connect=connected;c.on_message=message
try:
 summary['pre_stop']=stop();print('STOP',summary['pre_stop'],flush=True)
 c.connect('127.0.0.1',1883);c.loop_start();assert ready.wait(3)
 begin=time.monotonic();last=0;done_at=None;summary['start_t']=time.time();print('CAPTURE_READY_MOTORS_STOP',flush=True)
 while time.monotonic()-begin<180:
  if unsafe.is_set():raise RuntimeError('unexpected non-STOP mode')
  if signal.exists() and done_at is None:done_at=time.monotonic();summary['user_done_t']=time.time()
  if done_at is not None and time.monotonic()-done_at>=10:summary['reason']='user_done_plus_10s';break
  if time.monotonic()-begin-last>=5:
   last=time.monotonic()-begin
   payload={'elapsed_s':round(last,1),**live};tmp=Path('/private/tmp/cmcar-mag-calibration-live.tmp');tmp.write_text(json.dumps(payload));os.replace(tmp,'/private/tmp/cmcar-mag-calibration-live.json');print('STATUS',payload.get('mag'),flush=True)
  time.sleep(.2)
 else:summary['reason']='180s_capture_limit'
except Exception as e:
 summary['error']=repr(e);print('ERROR',repr(e),flush=True)
finally:
 summary['stop']=stop();summary['end_t']=time.time();summary['last']=live;time.sleep(.3);c.disconnect();c.loop_stop()
 (b/(name+'.jsonl.gz')).write_bytes(gzip.compress(''.join(json.dumps(e)+'\n' for e in events).encode(),mtime=0))
 (b/(name+'-summary.json')).write_text(json.dumps(summary,indent=2)+'\n');print('FINAL_STOP',summary['stop'],flush=True)
