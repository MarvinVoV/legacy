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
timer=threading.Timer(limit,stop)
try:
 summary['pre_stop']=stop();assert summary['pre_stop'].get('confirmed') and summary['pre_stop'].get('stopped'),summary
 state=request('/agent/devices/DEV_1/state');status=request('/devices/DEV_1/status');summary['before']={'state':state,'status':status};print('BEFORE',json.dumps(summary['before']),flush=True)
 assert state.get('online') and state.get('stm32_connected') and not state['motion']['stale'],state
 assert abs(state['velocity']['linear_cm_s'])<0.1,state
 assert status['data']['imu_age_s'] is not None and status['data']['imu_age_s']<1,status
 print('STATIC_CAPTURE_4S',flush=True);time.sleep(4);summary['before']={'state':request('/agent/devices/DEV_1/state'),'status':request('/devices/DEV_1/status')};print('AFTER_STATIC',json.dumps(summary['before']),flush=True)
 timer.start();summary['move']=request('/agent/devices/DEV_1/move',{'distance_cm':distance,'turn_deg':turn,'speed_cm_s':speed},post=True);print('MOVE',json.dumps(summary['move']),flush=True)
finally:
 summary['stop']=stop();timer.cancel();time.sleep(0.3);c.disconnect();c.loop_stop()
 Path('stm32_project/Docs/validation/path-tracking-2026-11-08/'+name+'.jsonl').write_text(''.join(json.dumps(x)+'\n' for x in events))
summary['after']=request('/agent/devices/DEV_1/state');print('AFTER',json.dumps(summary['after']),flush=True)
Path('stm32_project/Docs/validation/path-tracking-2026-11-08/'+name+'-summary.json').write_text(json.dumps(summary,indent=2))
print('HEAD_LOGS',json.dumps([e['data'] for e in events if e['topic'].endswith('/log') and 'HEAD' in str(e['data'])][-8:]),flush=True)
