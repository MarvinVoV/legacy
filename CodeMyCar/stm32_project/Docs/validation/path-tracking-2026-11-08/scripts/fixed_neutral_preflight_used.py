"""Ground use only after explicit 50cm clearance confirmation. Fixed neutral mode2/angular0.
Never uses nonzero differential yaw. Raw seq ranges avoid active agent seq 243+.
"""
import json,time,threading,gzip,urllib.request,math,sys
from pathlib import Path
import paho.mqtt.client as mqtt
if sys.argv[1:]!=['--ground-ready']:raise SystemExit('requires --ground-ready after user confirms space')
b=Path('stm32_project/Docs/validation/path-tracking-2026-11-08');n='fixed-neutral-1530-paired-20cm';events=[];summary={'legs':[]};ready=threading.Event();acks={};session=[None];changed=threading.Event()
assert not (b/(n+'.jsonl.gz')).exists()
def req(path,body=None,post=False):
 q=urllib.request.Request('http://127.0.0.1:8000/api/v1'+path,data=json.dumps(body).encode() if body is not None else None,headers={'Content-Type':'application/json'},method='POST' if post else 'GET')
 with urllib.request.urlopen(q,timeout=4) as f:return json.load(f)
def stop():
 t=time.monotonic();d=req('/agent/devices/DEV_1/stop',post=True);print('STOP',d,round((time.monotonic()-t)*1000,1),flush=True)
 assert all(d.get(k) for k in ('accepted','confirmed','stopped'));return d

def connected(c,u,f,r,p):c.subscribe('data/DEV_1/#');ready.set()
def message(c,u,m):
 try:d=json.loads(m.payload)
 except ValueError:return
 events.append({'t':time.time(),'topic':m.topic,'data':d})
 if m.topic.endswith('/ack'):acks[d['seq']]=d
 if m.topic.endswith('/motion'):
  ss=d.get('session_id')
  if session[0] is None:session[0]=ss
  elif ss!=session[0]:changed.set()
c=mqtt.Client(mqtt.CallbackAPIVersion.VERSION2,client_id='cmcar-fixed-neutral-baseline');c.on_connect=connected;c.on_message=message
watch=None;expired=threading.Event()
def deadline_stop():
 expired.set()
 try:stop()
 except Exception as e:print('WATCHDOG_STOP_ERROR',repr(e),flush=True)
try:
 summary['pre_stop']=stop();c.connect('127.0.0.1',1883);c.loop_start();assert ready.wait(3)
 for direction,seqbase in [(1,100),(-1,150)]:
  time.sleep(4)
  state=req('/agent/devices/DEV_1/state');status=req('/devices/DEV_1/status')
  assert state['online'] and state['stm32_connected'] and not state['motion']['stale'] and abs(state['velocity']['linear_cm_s'])<.1,state
  assert status['data']['imu_age_s']<.5 and status['data']['imu_status']==3,status
  leg={'direction':direction,'before':state,'before_status':status,'start_t':time.time(),'commands':[]};summary['legs'].append(leg)
  origin=state['pose']['odometer_cm'];yaw0=status['data']['yaw_deg'];expired.clear();started=time.monotonic();watch=threading.Timer(3.2,deadline_stop);watch.start();seq=seqbase;reason=None
  try:
   while True:
    elapsed=time.monotonic()-started
    assert not expired.is_set() and not changed.is_set(),'timeout/session changed'
    cur=req('/agent/devices/DEV_1/state');st=req('/devices/DEV_1/status')
    assert cur['online'] and cur['stm32_connected'] and cur['motion']['age_ms']<250 and not cur['motion']['stale'],cur
    assert st['data']['imu_age_s']<.5,st
    travel=direction*(cur['pose']['odometer_cm']-origin)
    if travel>=20:reason='distance_reached';break
    assert travel>-.5,'wrong travel direction'
    assert elapsed<.8 or travel>=1,'no encoder progress'
    assert abs(cur['velocity']['linear_cm_s'])<15,'overspeed'
    dyaw=(st['data']['yaw_deg']-yaw0+180)%360-180
    assert abs(dyaw)<5,'yaw excursion'
    if any(a.get('status')!=0 or not a.get('ack') for k,a in acks.items() if seqbase<=k<seq):raise RuntimeError('command rejected')
    assert seq<seqbase+40,'command budget'
    payload={'seq':seq,'cmd':'motion','motion':{'mode':2,'params':{'linear_vel':direction*100,'angular_vel':0}}}
    reply=req('/devices/DEV_1/commands',payload,True);assert reply.get('success'),reply
    leg['commands'].append({'t':time.time(),'seq':seq});seq+=1
    time.sleep(.1)
   leg['reason']=reason
  finally:
   leg['stop']=stop();watch.cancel();watch=None
  time.sleep(.4);leg['end_t']=time.time();leg['after']=req('/agent/devices/DEV_1/state');leg['after_status']=req('/devices/DEV_1/status')
  leg['signed_distance_cm']=leg['after']['pose']['odometer_cm']-origin
  leg['imu_ccw_change_deg']=(leg['after_status']['data']['yaw_deg']-yaw0+180)%360-180
  print('LEG',json.dumps(leg),flush=True)
  assert reason=='distance_reached' and abs(leg['after']['velocity']['linear_cm_s'])<.1
except Exception as e:
 summary['error']=repr(e);print('ERROR',repr(e),flush=True)
finally:
 if watch:watch.cancel()
 summary['stop']=stop();time.sleep(.3);c.disconnect();c.loop_stop()
 (b/(n+'.jsonl.gz')).write_bytes(gzip.compress(''.join(json.dumps(e)+'\n' for e in events).encode(),mtime=0))
 (b/(n+'-summary.json')).write_text(json.dumps(summary,indent=2)+'\n')
