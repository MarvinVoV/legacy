"""Offline 10Hz telemetry + IMU reconstruction, not external position truth."""
import gzip,json,re,math,bisect,statistics,sys
names=sys.argv[1:]
from pathlib import Path
b=Path('stm32_project/Docs/validation/path-tracking-2026-11-08');events=[]
for n in names:
 events += [json.loads(x) for x in gzip.decompress((b/(n+'.jsonl.gz')).read_bytes()).decode().splitlines()]
events.sort(key=lambda e:e['t']);yaw=[];motion=[];sessions=set()
for e in events:
 q=re.search(r'IMU yaw=(-?[\d.]+)',e['data'].get('message',''))
 if q:yaw.append((e['t'],float(q[1])))
 if e['topic'].endswith('/motion'):
  motion.append((e['t'],e['data']['odometer_mm'],e['data']['state']));sessions.add(e['data']['session_id'])
assert len(sessions)==1
T=[p[0] for p in yaw]
def angle(t):
 j=bisect.bisect_left(T,t)
 if j==0:return yaw[0][1]
 if j==len(T):return yaw[-1][1]
 a,c=yaw[j-1],yaw[j];return a[1]+(c[1]-a[1])*(t-a[0])/(c[0]-a[0])
x=y=0.;maxds=0.;gap=0.;ref=angle(motion[0][0])
for a,c in zip(motion,motion[1:]):
 ds=c[1]-a[1]
 if ds:
  theta=math.radians((angle(a[0])+angle(c[0]))/2-ref)
  x+=ds*math.cos(theta);y+=ds*math.sin(theta);maxds=max(maxds,abs(ds));gap=max(gap,c[0]-a[0])
summaries=[json.loads((b/(n+'-summary.json')).read_text()) for n in names]
legs=[l for s in summaries for l in s['legs']];idealx=idealy=0.;h=0.
for leg in legs:
 z=leg['move']['measured'];s=z['distance_cm']*10;a=-z['turn_deg']*math.pi/180
 dx=s*math.sin(a)/a if abs(a)>1e-8 else s;dy=s*(1-math.cos(a))/a if abs(a)>1e-8 else 0
 idealx+=dx*math.cos(h)-dy*math.sin(h);idealy+=dx*math.sin(h)+dy*math.cos(h);h+=a
out={'telemetry_imu_closure_forward_mm':x,'telemetry_imu_closure_left_mm':y,'max_nonzero_distance_step_mm':maxds,'max_receive_gap_for_nonzero_distance_step_s':gap,'approx_ideal_constant_curvature_closure_forward_mm':idealx,'approx_ideal_constant_curvature_closure_left_mm':idealy,'sum_goal_turn_ccw_deg':h*180/math.pi,'captures':names,'limitations':'Rounded telemetry and host receive-time interpolation; includes recorded coasting, capture gap assumed stationary. Wheel mean distance and IMU yaw still assumed. Ideal arcs use terminal measured values and assume constant curvature, excludes extra coasting. Neither estimate is external truth.'}
(b/(names[0]+'-full-closure-estimate.json')).write_text(json.dumps(out,indent=2)+'\n');print(json.dumps(out,indent=2))
