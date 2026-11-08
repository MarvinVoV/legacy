"""Offline analysis of a two-leg GOAL capture. Estimates are not position truth.
Usage: python analyze_ground_load_pair.py PREFIX
"""
import gzip,json,re,statistics,sys,math
from pathlib import Path
b=Path(__file__).resolve().parent.parent
name=sys.argv[1]
s=json.loads((b/(name+'-summary.json')).read_text())
p=b/(name+'.jsonl')
raw=p.read_text() if p.exists() else gzip.decompress(Path(str(p)+'.gz').read_bytes()).decode()
a=[json.loads(line) for line in raw.splitlines()]
rows=[]
for leg in s['legs']:
    seq=leg['move']['seq']
    moving=[e for e in a if e['topic'].endswith('/motion') and e['data'].get('seq')==seq and e['data'].get('state')=='running']
    logs=[e for e in a if leg['start_t']<=e['t']<=leg['end_t'] and e['topic'].endswith('/log')]
    heads=[e['data']['message'] for e in logs if e['data'].get('message','').startswith('HEAD ') and 'v=0.000' not in e['data']['message']]
    ref=float(re.search(r'ref=(-?[0-9.]+)',heads[0])[1]) if heads else None
    ys=[];wheels={w:[] for w in 'LR'};bins={k:{w:[] for w in 'LR'} for k in ('0.4_to_3s','3_to_8s','after_8s')}
    for e in logs:
        msg=e['data'].get('message','')
        q=re.search(r'IMU yaw=(-?[0-9.]+)',msg)
        if q and moving and moving[0]['t']<=e['t']<=moving[-1]['t'] and ref is not None:
            ys.append(math.remainder(float(q[1])-ref,360))
        q=re.search(r'MOT_([LR]) tgt=(-?[0-9.]+) act=(-?[0-9.]+)',msg)
        if q and moving and e['t']>=moving[0]['t']+.4 and abs(float(q[2]))>25:
            error=float(q[3])-float(q[2]);wheels[q[1]].append(error)
            elapsed=e['t']-moving[0]['t']
            label='0.4_to_3s' if elapsed<3 else '3_to_8s' if elapsed<8 else 'after_8s'
            bins[label][q[1]].append(error)
    row={'distance_cm':leg['distance_cm'],'seq':seq,'measured':leg['move']['measured'],
         'peak_relative_yaw_10hz_deg':max(map(abs,ys)) if ys else None,
         'wheel_tracking_2hz_samples_above_25rpm':{w:{'samples':len(v),'mean_actual_minus_target_rpm':statistics.mean(v) if v else None} for w,v in wheels.items()},
         'wheel_tracking_time_bins':{k:{w:{'samples':len(v),'mean_actual_minus_target_rpm':statistics.mean(v) if v else None} for w,v in pair.items()} for k,pair in bins.items()},
         'last_path_log':next((e['data']['message'] for e in reversed(logs) if e['data'].get('message','').startswith('PATH ')),None)}
    rows.append(row)
result={'capture':name,'legs':rows,'final_stop':s['stop'],'error':s.get('error'),'limitations':'Short transient windows, rounded 2Hz RPM and 10Hz yaw; no external lateral-position measurement.'}
(b/(name+'-analysis.json')).write_text(json.dumps(result,indent=2)+'\n')
print(json.dumps(result,indent=2))
