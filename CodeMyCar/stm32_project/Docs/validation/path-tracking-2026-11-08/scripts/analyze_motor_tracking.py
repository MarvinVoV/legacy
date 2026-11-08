import json,gzip,re,statistics,math
from pathlib import Path
b=Path('stm32_project/Docs/validation/path-tracking-2026-11-08');out=[]
for n in ['path-rpm-forward-200cm-speed30','path-off-control-reverse-200cm-speed30','dynamic-neutral-roundtrip-200cm-speed10']:
 s=json.loads((b/(n+'-summary.json')).read_text());p=b/(n+'.jsonl');raw=p.read_text() if p.exists() else gzip.decompress(Path(str(p)+'.gz').read_bytes()).decode();events=[json.loads(l) for l in raw.splitlines()];legs=s.get('legs') or [{'move':s['move']}]
 for leg in legs:
  seq=leg['move']['seq'];m=[e for e in events if e['topic'].endswith('/motion') and e['data'].get('seq')==seq and e['data'].get('state')=='running'];lo=m[0]['t']+1;hi=m[-1]['t']-1
  r={'capture':n,'seq':seq,'distance_cm':leg['move']['measured']['distance_cm'],'window_received_s':[lo,hi],'wheels':{}}
  for wheel in 'LR':
   samples=[]
   for e in events:
    if not lo<=e['t']<=hi:continue
    q=re.search(r'MOT_'+wheel+r' tgt=(-?[\d.]+) act=(-?[\d.]+) duty=(-?[\d.]+) pulses=(-?\d+)',e['data'].get('message',''))
    if q:samples.append({'tick_s':e['data']['timestamp']/1000,'target':float(q[1]),'actual':float(q[2]),'duty':float(q[3]),'pulses':int(q[4])})
   if len(samples)<3:continue
   errors=[x['actual']-x['target'] for x in samples];dt=samples[-1]['tick_s']-samples[0]['tick_s']
   actual=(samples[-1]['pulses']-samples[0]['pulses'])/1560/dt*60
   target=sum(.5*(a['target']+z['target'])*(z['tick_s']-a['tick_s']) for a,z in zip(samples,samples[1:]))/dt
   r['wheels'][wheel]={'samples':len(samples),'mean_target_rpm':statistics.mean(x['target'] for x in samples),'mean_actual_rpm':statistics.mean(x['actual'] for x in samples),'mean_actual_minus_target_rpm':statistics.mean(errors),'rms_tracking_error_rpm':math.sqrt(statistics.mean(x*x for x in errors)),'minmax_error_rpm':[min(errors),max(errors)],'mean_duty':statistics.mean(x['duty'] for x in samples),'pulse_count_average_rpm':actual,'sampled_target_trapezoid_average_rpm':target,'pulse_average_minus_sampled_target_rpm':actual-target}
  out.append(r)
(b/'motor-tracking-offline-analysis.json').write_text(json.dumps(out,indent=2)+'\n');print(json.dumps(out,indent=2))
