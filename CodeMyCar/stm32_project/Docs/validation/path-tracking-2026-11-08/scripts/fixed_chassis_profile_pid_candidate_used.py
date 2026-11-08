"""Only for confirmed fixed chassis with BOTH drive wheels suspended.
No distance/yaw pose is a physical movement measurement during this test.
"""
import json,time,gzip,threading,urllib.request,sys
from pathlib import Path
import paho.mqtt.client as mqtt
if sys.argv[1:]!=['--suspended-ready']:raise SystemExit('requires confirmed fixed suspended chassis')
b=Path('stm32_project/Docs/validation/path-tracking-2026-11-08');name='fixed-chassis-speed-profile-02';events=[];summary={'phases':[]};ready=threading.Event();bad=threading.Event();expired=threading.Event();timer=None;session=[None];active=[False];limit=[500]
abort=Path('/private/tmp/cmcar-fixed-chassis-abort-02');assert not abort.exists() and not (b/(name+'.jsonl.gz')).exists()
def req(path,d=None,post=False):
 q=urllib.request.Request('http://127.0.0.1:8000/api/v1'+path,data=json.dumps(d).encode() if d is not None else None,headers={'Content-Type':'application/json'},method='POST' if post else 'GET')
 with urllib.request.urlopen(q,timeout=4) as f:return json.load(f)
def stop():
 s=req('/agent/devices/DEV_1/stop',post=True);print('STOP',s,flush=True);assert all(s.get(k) for k in ('accepted','confirmed','stopped'));return s
def deadline():
 expired.set()
 try:stop()
 except Exception as e:print('WATCHDOG_STOP_ERROR',repr(e),flush=True)
def connected(c,u,f,r,p):c.subscribe('data/DEV_1/#');ready.set()
def message(c,u,m):
 try:d=json.loads(m.payload)
 except ValueError:return
 events.append({'t':time.time(),'topic':m.topic,'data':d})
 if m.topic.endswith('/motion'):
  ss=d.get('session_id')
  if session[0] is None:session[0]=ss
  elif ss!=session[0]:bad.set()
  if d.get('fault') or d.get('error') or (active[0] and max(abs(d.get('left_mmps',0)),abs(d.get('right_mmps',0)))>limit[0]):bad.set()
 if m.topic.endswith('/ack') and d.get('seq',0)>=64 and d.get('status')!=0:bad.set()
 if active[0] and any(v in d.get('message','') for v in ('IMU ready','reopen SH-2','IMU probe:')):bad.set()
c=mqtt.Client(mqtt.CallbackAPIVersion.VERSION2,client_id='cmcar-fixed-chassis-speed-profile');c.on_connect=connected;c.on_message=message
try:
 summary['pre_stop']=stop();c.connect('127.0.0.1',1883);c.loop_start();assert ready.wait(3);time.sleep(5)
 for speed in [100,300,-100,-300]:
  st=req('/agent/devices/DEV_1/state');is_=req('/devices/DEV_1/status');assert st['online'] and st['stm32_connected'] and abs(st['velocity']['linear_cm_s'])<.1;assert is_['data']['imu_age_s']<.5
  assert not bad.is_set() and not abort.exists()
  phase={'speed_mm_s':speed,'before':st,'before_status':is_,'start_t':time.time()};summary['phases'].append(phase)
  yaw0=is_['data']['yaw_deg'];started=time.monotonic();seq=64;expired.clear();limit[0]=200 if abs(speed)==100 else 450;active[0]=True;timer=threading.Timer(10,deadline);timer.start()
  try:
   while time.monotonic()-started<8:
    elapsed=time.monotonic()-started
    assert not expired.is_set() and not bad.is_set() and not abort.exists(),'timeout/fault/overspeed/session/reopen/abort'
    st=req('/agent/devices/DEV_1/state');is_=req('/devices/DEV_1/status');assert st['online'] and st['stm32_connected'] and not st['motion']['stale'] and st['motion']['age_ms']<(300 if elapsed>.35 else 1500)
    assert is_['data']['imu_age_s']<.5
    assert abs((is_['data']['yaw_deg']-yaw0+180)%360-180)<3,'frame motion or excessive IMU drift'
    assert seq<165
    p={'seq':seq,'cmd':'motion','motion':{'mode':2,'params':{'linear_vel':speed,'angular_vel':0}}};assert req('/devices/DEV_1/commands',p,True)['success'];seq+=1;time.sleep(.1)
   phase['end_t']=time.time()
  finally:
   active[0]=False;phase['stop']=stop();timer.cancel();timer=None
  assert not expired.is_set()
  time.sleep(5);phase['after_status']=req('/devices/DEV_1/status');print('PHASE_DONE',speed,json.dumps(phase['after_status']['data']),flush=True)
except Exception as e:summary['error']=repr(e);print('ERROR',repr(e),flush=True)
finally:
 active[0]=False
 if timer:timer.cancel()
 summary['stop']=stop();time.sleep(.5);summary['after']=req('/agent/devices/DEV_1/state');c.disconnect();c.loop_stop()
 (b/(name+'.jsonl.gz')).write_bytes(gzip.compress(''.join(json.dumps(e)+'\n' for e in events).encode(),mtime=0));(b/(name+'-summary.json')).write_text(json.dumps(summary,indent=2)+'\n');print('FINAL',json.dumps(summary['after']),flush=True)
