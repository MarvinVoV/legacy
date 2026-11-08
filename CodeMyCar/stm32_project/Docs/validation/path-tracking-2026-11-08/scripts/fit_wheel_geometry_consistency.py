"""Read-only consistency fit; never writes calibration to vehicle/source."""
import gzip,json,re,math,bisect
from pathlib import Path
b=Path('stm32_project/Docs/validation');out=b/'path-tracking-2026-11-08';rows=[]
def interp(a,t):
 i=bisect.bisect_left([v[0] for v in a],t)
 if i==0:return a[0][1]
 if i==len(a):return a[-1][1]
 x,y=a[i-1],a[i];return x[1]+(y[1]-x[1])*(t-x[0])/(y[0]-x[0])
def row(path,label,lo=-math.inf,hi=math.inf):
 a=[json.loads(l) for l in gzip.decompress(path.read_bytes()).decode().splitlines()];v={'L':[],'R':[]};yaw=[]
 for e in a:
  m=e['data'].get('message','');t=e['data'].get('timestamp',0)/1000
  q=re.search(r'IMU yaw=(-?[\d.]+)',m)
  if q:yaw.append((t,float(q[1])))
  q=re.search(r'MOT_([LR]).*pulses=(-?\d+)',m)
  if q and lo<=e['t']<=hi:v[q[1]].append((t,int(q[2])))
 if min(len(v['L']),len(v['R']),len(yaw))<2:raise ValueError(label)
 t0=max(v['L'][0][0],v['R'][0][0],yaw[0][0]);t1=min(v['L'][-1][0],v['R'][-1][0],yaw[-1][0]);assert t1>t0
 dl=(interp(v['L'],t1)-interp(v['L'],t0))*2*math.pi*32.5/1560;dr=(interp(v['R'],t1)-interp(v['R'],t0))*2*math.pi*32.5/1560
 dy=interp(yaw,t1)-interp(yaw,t0)
 rows.append({'capture':label,'duration_s':t1-t0,'left_nominal_mm':dl,'right_nominal_mm':dr,'grv_yaw_delta_deg':dy,'nominal_encoder_ccw_deg':(dr-dl)/167*180/math.pi,'A':[-dl/167,dr/167],'y':dy*math.pi/180})
for n in ['left-60cm-30deg','right-60cm-30deg','reverse-left-60cm-30deg','reverse-right-60cm-30deg']:row(b/'ground-2026-11-08'/(n+'.jsonl.gz'),n)
def fit(rs):
 aa=sum(r['A'][0]**2 for r in rs);bb=sum(r['A'][0]*r['A'][1] for r in rs);cc=sum(r['A'][1]**2 for r in rs);dy=sum(r['A'][0]*r['y'] for r in rs);ey=sum(r['A'][1]*r['y'] for r in rs);d=aa*cc-bb*bb;return [(dy*cc-bb*ey)/d,(aa*ey-bb*dy)/d]
fits={label:fit(rs) for label,rs in [('all_arcs',rows),('forward_arcs',rows[:2]),('reverse_arcs',rows[2:])]}
for n in ['ground-load-roundtrip-200cm-speed10-01','ground-no-trim-roundtrip-200cm-speed10-01']:
 s=json.loads((out/(n+'-summary.json')).read_text())
 for l in s['legs']:row(out/(n+'.jsonl.gz'),n+':'+str(l['distance_cm']),l['start_t'],l['end_t'])
for r in rows:
 r['residuals_deg']={label:(r['A'][0]*k[0]+r['A'][1]*k[1]-r['y'])*180/math.pi for label,k in fits.items()};del r['A'];del r['y']
d={'fit_factors_left_right':fits,'rows':rows,'limitations':'Exploratory consistency fit, not true radius calibration. Historic arc firmware differs; rounded logs interpolated by MCU time. Fixed track=167mm and GRV yaw assumed. Slip, effective track and yaw error not identifiable independently. Do not apply these constants.'};(out/'wheel-geometry-consistency-fit.json').write_text(json.dumps(d,indent=2)+'\n');print(json.dumps(d,indent=2))
