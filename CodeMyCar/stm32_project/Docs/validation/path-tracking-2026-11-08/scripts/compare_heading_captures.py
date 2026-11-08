"""Offline: compare straight-GOAL captures from this directory (no hardware).
Usage: python compare_heading_captures.py PREFIX [PREFIX ...]
Peak = 10Hz, rounded IMU logs within received running-status interval;
not an independent ground-position or continuous-time peak measurement.
"""
import gzip
import json
import math
from pathlib import Path
import re
import sys
b=Path(__file__).resolve().parent.parent
rows=[]
for n in sys.argv[1:]:
    s=json.loads((b/(n+'-summary.json')).read_text())
    p=b/(n+'.jsonl')
    raw=p.read_text() if p.exists() else gzip.decompress(Path(str(p)+'.gz').read_bytes()).decode()
    events=[json.loads(line) for line in raw.splitlines()]
    motion=[e for e in events if e['topic'].endswith('/motion') and
            e['data'].get('seq')==s['move']['seq'] and e['data'].get('state')=='running']
    logs=[e for e in events if e['topic'].endswith('/log')]
    heads=[e['data']['message'] for e in logs if e['data'].get('message','').startswith('HEAD ')]
    ref=float(re.search(r'ref=(-?[0-9.]+)',heads[-1])[1])
    deviations=[]
    for e in logs:
        q=re.search(r'IMU yaw=(-?[0-9.]+)',e['data'].get('message',''))
        if q and motion[0]['t']<=e['t']<=motion[-1]['t']:
            deviations.append(math.remainder(float(q[1])-ref,360))
    last_path=[e['data']['message'] for e in logs if e['data'].get('message','').startswith('PATH ')][-1]
    rows.append({'capture':n,'measured':s['move']['measured'],
                 'peak_relative_yaw_from_10hz_logs_deg':max(map(abs,deviations)),
                 'last_path_log':last_path})
print(json.dumps(rows,indent=2))
