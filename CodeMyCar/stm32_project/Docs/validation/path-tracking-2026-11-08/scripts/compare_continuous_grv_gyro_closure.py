"""Offline GRV vs continuous gyro closure; neither is external position truth."""
import gzip,json,re,math,bisect,statistics,sys
from pathlib import Path
b=Path('stm32_project/Docs/validation/path-tracking-2026-11-08')
for name in sys.argv[1:]:
 a=[json.loads(x) for x in gzip.decompress((b/(name+'.jsonl.gz')).read_bytes()).decode().splitlines()];q=[];mot=[]
 for e in a:
  m=re.search(r'IMU yaw=(-?[\d.]+) deg rate=(-?[\d.]+)',e['data'].get('message',''))
  if m:q.append({'rx':e['t'],'source':e['data']['timestamp']/1000,'grv':float(m[1]),'rate':float(m[2])})
  if e['topic'].endswith('/motion'):mot.append((e['t'],e['data']['odometer_mm'],e['data']['state']))
 assert len(q)>10 and len(mot)>10
 for bias in (0,-.005,.005):
  for i,x in enumerate(q):x['gyro']=0 if not i else q[i-1]['gyro']+(x['source']-q[i-1]['source'])*(q[i-1]['rate']+x['rate']-2*bias)*.5
  times=[x['rx'] for x in q]
  def interp(t,key):
   j=bisect.bisect_left(times,t)
   if j==0:return q[0][key]
   if j==len(q):return q[-1][key]
   x,y=q[j-1],q[j];return x[key]+(y[key]-x[key])*(t-x['rx'])/(y['rx']-x['rx'])
  result={}
  for key in ('grv','gyro'):
   ref=interp(mot[0][0],key);xf=yf=0
   for x,y in zip(mot,mot[1:]):
    d=y[1]-x[1];h=math.radians((interp(x[0],key)+interp(y[0],key))*.5-ref)
    xf+=d*math.cos(h);yf+=d*math.sin(h)
   result[key]={'forward_mm':xf,'left_mm':yf,'yaw_change_deg':q[-1][key]-q[0][key]}
  print(name,'bias_dps',bias,json.dumps(result))
