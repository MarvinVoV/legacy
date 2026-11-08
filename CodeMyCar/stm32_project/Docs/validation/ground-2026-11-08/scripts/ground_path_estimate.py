import json,math,bisect,sys
from pathlib import Path
if '--help' in sys.argv or len(sys.argv)!=2:
 print('Offline only: run from CodeMyCar root with successful capture prefix; supports .jsonl or .jsonl.gz; writes derived -path-estimate.json')
 raise SystemExit(0 if '--help' in sys.argv else 2)
base=Path('stm32_project/Docs/validation/ground-2026-11-08');name=sys.argv[1]
s=json.loads((base/(name+'-summary.json')).read_text());import gzip;cap=base/(name+'.jsonl');raw=cap.read_text() if cap.exists() else gzip.decompress(cap.with_name(cap.name+'.gz').read_bytes()).decode();r=[json.loads(x) for x in raw.splitlines()]
m=[x for x in r if x['topic'].endswith('/motion')];running=[x for x in m if x['data'].get('mode')==4 and x['data'].get('state')=='running'];start=running[0]['t'];seq=running[0]['data']['seq'];end=next(x['t'] for x in m if x['data'].get('seq')==seq and x['data'].get('state')=='done')
h=[x for x in r if x['topic'].endswith('/sensor') and x['data'].get('type')=='heading'];ref=next((x['data']['data']['yaw_mdeg']/1000 for x in reversed(h) if x['t']<=start),s['before']['status']['data']['yaw_deg'])
def diff(a,b):return (a-b+180)%360-180
active=[x for x in h if start<=x['t']<=end];peak=max(active,key=lambda x:abs(diff(x['data']['data']['yaw_mdeg']/1000,ref)))
ht=[x['t'] for x in h];ha=[diff(x['data']['data']['yaw_mdeg']/1000,ref) for x in h]
def angle(t):
 i=bisect.bisect_left(ht,t)
 if i==0:return ha[0]
 if i==len(ht):return ha[-1]
 f=(t-ht[i-1])/(ht[i]-ht[i-1]);return ha[i-1]+f*(ha[i]-ha[i-1])
odo=s['before']['state']['pose']['odometer_cm']*10;prev_t=start;x=y=0;path=[]
for e in m:
 if e['t']<start:continue
 ds=(e['data']['odometer_mm']-odo)/10;odo=e['data']['odometer_mm'];a=math.radians(angle((e['t']+prev_t)/2));x+=ds*math.cos(a);y-=ds*math.sin(a);prev_t=e['t'];path.append({'elapsed_s':e['t']-start,'forward_cm':x,'right_cm':y})
before=s['before']['state']['pose'];after=s['after']['pose'];theta=math.radians(before['heading_deg']);dx=after['x_cm']-before['x_cm'];dy=after['y_cm']-before['y_cm']
out={'reference_yaw_deg':ref,'heading_samples':len(active),'max_abs_yaw_deviation_deg':abs(diff(peak['data']['data']['yaw_mdeg']/1000,ref)),'peak_right_yaw_deg':-diff(peak['data']['data']['yaw_mdeg']/1000,ref),'peak_after_start_s':round(peak['t']-start,3),'last_running_right_yaw_deg':-diff(active[-1]['data']['data']['yaw_mdeg']/1000,ref),'post_stop_right_yaw_deg':-diff(h[-1]['data']['data']['yaw_mdeg']/1000,ref),'encoder_imu_estimated_forward_cm':x,'encoder_imu_estimated_final_right_cm':y,'encoder_imu_estimated_max_abs_lateral_cm':max(abs(p['right_cm']) for p in path),'encoder_only_pose_lateral_cm':-math.sin(theta)*dx+math.cos(theta)*dy,'goal_reported_turn_deg':s['move']['measured']['turn_deg'],'method':'encoder odometer increments projected using midpoint interpolated IMU yaw relative to starting yaw; MQTT receive-time alignment; no external position truth'}
(base/(name+'-path-estimate.json')).write_text(json.dumps({'summary':out,'path':path},indent=2));print(json.dumps(out,indent=2))
