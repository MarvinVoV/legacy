import gzip,json,re,statistics,math
from pathlib import Path
b=Path('stm32_project/Docs/validation/path-tracking-2026-11-08');n='fixed-chassis-direct-profile-03';s=json.loads((b/(n+'-summary.json')).read_text());a=[json.loads(l) for l in gzip.decompress((b/(n+'.jsonl.gz')).read_bytes()).decode().splitlines()];rows=[]
for p in s['phases']:
 lo=p['start_t'];hi=p['end_t'];ys=[];rates=[];mot={'L':[],'R':[]}
 for e in a:
  if not lo+1<=e['t']<=hi:continue
  m=e['data'].get('message','');q=re.search(r'IMU yaw=(-?[\d.]+) deg rate=(-?[\d.]+)',m)
  if q:ys.append((e['data']['timestamp']/1000,float(q[1])));rates.append(float(q[2]))
  q=re.search(r'MOT_([LR]) tgt=(-?[\d.]+) act=(-?[\d.]+) duty=(-?[\d.]+) pulses=(-?\d+)',m)
  if q:mot[q[1]].append({'elapsed':e['t']-lo,'target':float(q[2]),'actual':float(q[3]),'duty':float(q[4]),'pulses':int(q[5]),'source_ms':e['data']['timestamp']})
 tt=[p[0]-ys[0][0] for p in ys];yy=[p[1] for p in ys];mt=statistics.mean(tt);my=statistics.mean(yy);slope=sum((t-mt)*(y-my) for t,y in zip(tt,yy))/sum((t-mt)**2 for t in tt)
 row={'speed_mm_s':p['speed_mm_s'],'yaw_range_deg':[min(yy),max(yy)],'yaw_linear_drift_dps':slope,'gyro_yaw_rate_mean_dps':statistics.mean(rates),'wheels':{}}
 for c in 'LR':
  row['wheels'][c]={}
  for label,f in [('all_after_1s',lambda x:True),('late_after_15s',lambda x:x['elapsed']>=15)]:
   z=[x for x in mot[c] if f(x)];er=[x['actual']-x['target'] for x in z]
   row['wheels'][c][label]={'samples':len(z),'mean_target':statistics.mean(x['target'] for x in z),'mean_actual':statistics.mean(x['actual'] for x in z),'mean_error':statistics.mean(er),'rms_error':math.sqrt(statistics.mean(v*v for v in er))}
  z=[x for x in mot[c] if x['elapsed']>=5]
  if len(z)>=2:
   a0,a1=z[0],z[-1];dt=(a1['source_ms']-a0['source_ms'])/1000
   avg=(a1['pulses']-a0['pulses'])/1560/dt*60
   row['wheels'][c]['pulse_average_after_5s']={'duration_s':dt,'rpm':avg,'mean_target':statistics.mean(x['target'] for x in z),'error':avg-statistics.mean(x['target'] for x in z)}
 rows.append(row)
(b/(n+'-analysis.json')).write_text(json.dumps(rows,indent=2)+'\n');print(json.dumps(rows,indent=2))
