"""Diagnostic sphere fit to existing calibrated-field logs; no hardware writes.
Not a replacement calibration: sensor-side calibration may change during capture.
"""
import gzip,json,math,statistics
from pathlib import Path
b=Path('stm32_project/Docs/validation/path-tracking-2026-11-08');a=[json.loads(l) for l in gzip.decompress((b/'mag-manual-calibration-01.jsonl.gz').read_bytes()).decode().splitlines()];points=[]
for e in a:
 m=e['data'].get('message','')
 if not m.startswith('IMU_FIELD '):continue
 d=dict(v.split('=',1) for v in m.split()[1:] if '=' in v)
 v=[float(d[k]) for k in ('x','y','z')]
 if all(math.isfinite(x) for x in v):points.append(v)
# Equalize coarse spatial bins, so stationary dwell does not dominate.
bins={}
for p in points:bins.setdefault(tuple(round(v/5) for v in p),[]).append(p)
pts=[[statistics.mean(p[i] for p in group) for i in range(3)] for group in bins.values()]
normal=[[0.]*5 for _ in range(4)]
for p in pts:
 row=[2*p[0],2*p[1],2*p[2],1];rhs=sum(v*v for v in p)
 for i in range(4):
  for j in range(4):normal[i][j]+=row[i]*row[j]
  normal[i][4]+=row[i]*rhs
for i in range(4):
 k=max(range(i,4),key=lambda k:abs(normal[k][i]));normal[i],normal[k]=normal[k],normal[i]
 assert abs(normal[i][i])>1e-8,'singular orientation coverage'
 q=normal[i][i];normal[i]=[v/q for v in normal[i]]
 for k in range(4):
  if k==i:continue
  q=normal[k][i];normal[k]=[x-q*y for x,y in zip(normal[k],normal[i])]
c=[normal[i][4] for i in range(3)];rad2=normal[3][4]+sum(v*v for v in c);assert rad2>0
radius=math.sqrt(rad2)
r=[math.sqrt(sum((x-y)**2 for x,y in zip(p,c)))-radius for p in pts]
out={'input_points':len(points),'spatial_bins_5uT':len(pts),'coordinate_ranges_uT':[[min(p[i] for p in points),max(p[i] for p in points)] for i in range(3)],'sphere_center_uT':c,'sphere_radius_uT':radius,'spatial_bin_residual_rmse_uT':math.sqrt(statistics.mean(v*v for v in r)),'spatial_bin_residual_absmax_uT':max(map(abs,r)),'limits':'Exploratory sphere fit of sensor CALIBRATED field, not raw uncalibrated data. Uneven pose coverage, changing internal calibration, soft iron and spatial field gradients are not distinguished. Never applied to control or written to sensor.'}
(b/'mag-field-sphere-fit-diagnostic.json').write_text(json.dumps(out,indent=2)+'\n');print(json.dumps(out,indent=2))
