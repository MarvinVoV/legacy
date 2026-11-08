"""Offline first-DONE telemetry comparison. Rounded speed is not ground truth.
Usage: python compare_goal_done_packets.py PREFIX [PREFIX ...]
"""
import gzip,json,sys
from pathlib import Path
b=Path(__file__).resolve().parent.parent
rows=[]
for n in sys.argv[1:]:
 p=b/(n+'.jsonl');raw=p.read_text() if p.exists() else gzip.decompress(Path(str(p)+'.gz').read_bytes()).decode()
 a=[json.loads(line) for line in raw.splitlines()];s=json.loads((b/(n+'-summary.json')).read_text());r={'capture':n,'legs':[]}
 for leg in s['legs']:
  seq=leg['move']['seq'];packets=[e for e in a if e['topic'].endswith('/motion') and e['data'].get('seq')==seq]
  done=next((e['data'] for e in packets if e['data'].get('state')=='done'),None)
  r['legs'].append({'seq':seq,'reported_result':leg['move']['measured'],'first_done_packet':done})
 rows.append(r)
(b/'goal-settle-done-packet-comparison.json').write_text(json.dumps(rows,indent=2)+'\n')
for r in rows:
 for leg in r['legs']:
  d=leg['first_done_packet']
  print(r['capture'],leg['seq'],None if not d else {'left_mmps':d['left_mmps'],'right_mmps':d['right_mmps'],'goal_distance_mm':d['goal_distance_mm']})
