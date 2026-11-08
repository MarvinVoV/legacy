import json,time,threading,urllib.request,urllib.error,sys
from pathlib import Path
import paho.mqtt.client as mqtt
name,distance,turn,speed,limit=sys.argv[1:];distance=int(distance);turn=int(turn);speed=int(speed);limit=float(limit)
base='http://127.0.0.1:8000/api/v1';events=[];ready=threading.Event();summary={}
def request(path,body=None,post=False):
 q=urllib.request.Request(base+path,data=json.dumps(body).encode() if body else None,headers={'Content-Type':'application/json'},method='POST' if post else 'GET')
 try:
  with urllib.request.urlopen(q,timeout=limit+3) as r:return json.load(r)
 except urllib.error.HTTPError as e:return {'http_error':e.code,'body':e.read().decode()}
def stop():
 try:
  t=time.monotonic();r=request('/agent/devices/DEV_1/stop',post=True);print('STOP',json.dumps(r),'ms',round((time.monotonic()-t)*1000,1),flush=True);return r
 except Exception as e:print('STOP_ERROR',repr(e),flush=True);return {}
def connected(c,u,f,r,p):c.subscribe('data/DEV_1/#');ready.set()
def message(c,u,m):
 try:events.append({'t':time.time(),'topic':m.topic,'data':json.loads(m.payload)})
 except ValueError:pass
c=mqtt.Client(mqtt.CallbackAPIVersion.VERSION2,client_id='cmcar-ground-'+name);c.on_connect=connected;c.on_message=message
c.connect('127.0.0.1',1883);c.loop_start();assert ready.wait(3)
timer=None
out=Path('stm32_project/Docs/validation/path-tracking-2026-11-08')
try:
 summary['pre_stop']=stop();assert all(summary['pre_stop'].get(k) for k in ('accepted','confirmed','stopped')),summary
 state=request('/agent/devices/DEV_1/state');status=request('/devices/DEV_1/status')
 assert state.get('online') and state.get('stm32_connected') and not state['motion']['stale'],state
 assert abs(state['velocity']['linear_cm_s'])<0.1,state
 assert status['data']['imu_age_s'] is not None and status['data']['imu_age_s']<1,status
 summary['before']={'state':state,'status':status};print('STATIC_CAPTURE_4S',flush=True);time.sleep(4)
 summary['legs']=[]
 for d in (200,-200):
  leg={'distance_cm':d,'start_t':time.time(),'before_status':request('/devices/DEV_1/status')};summary['legs'].append(leg)
  timer=threading.Timer(limit,stop);timer.start()
  try:
   leg['move']=request('/agent/devices/DEV_1/move',{'distance_cm':d,'turn_deg':0,'speed_cm_s':10},post=True)
   print('MOVE',d,json.dumps(leg['move']),flush=True)
  finally:
   leg['stop']=stop();timer.cancel()
  leg['end_t']=time.time()
  assert leg['move'].get('ok') and leg['move'].get('measured',{}).get('final_state')=='done',leg
  assert all(leg['stop'].get(k) for k in ('accepted','confirmed','stopped')),leg
  st=request('/agent/devices/DEV_1/state');assert abs(st['velocity']['linear_cm_s'])<0.1,st
  time.sleep(2)
except Exception as e:
 summary['error']=repr(e);print('ERROR',repr(e),flush=True)
finally:
 summary['stop']=stop()
 if timer:timer.cancel()
 time.sleep(.3);c.disconnect();c.loop_stop()
 out.joinpath(name+'.jsonl').write_text(''.join(json.dumps(x)+'\n' for x in events))
 summary['after']=request('/agent/devices/DEV_1/state')
 out.joinpath(name+'-summary.json').write_text(json.dumps(summary,indent=2))
 print('AFTER',json.dumps(summary['after']),flush=True)
print('HEAD_LOGS',json.dumps([e['data'] for e in events if e['topic'].endswith('/log') and 'HEAD' in str(e['data'])][-8:]),flush=True)
