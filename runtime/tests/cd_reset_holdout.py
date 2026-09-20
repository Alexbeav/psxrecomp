"""Fresh reset cases and follow-up states taken from baseline observations."""
import itertools,json,random,sys
from pathlib import Path
E=Path(sys.argv[1]);D=json.loads(Path(sys.argv[2]).read_text())['fields'];rng=random.Random(920172);cases=[]
def add(s):
 if any(not D[k][0]<=v<=D[k][1] for k,v in s.items()):return
 if s['hold']:s['target']=min(s['target'],2147483645)
 if s['source_clock'] and s['drive'] and s['reset_due'] and s['clock']>=s['reset_due']:
  s['clock']=min(s['clock'],s['reset_due']+4095*225792)
 cases.append(dict(id=str(len(cases)),state=s))
for n in range(1300):
 s={k:rng.randint(lo,hi) for k,(lo,hi) in D.items()};s.update(source_clock=rng.choice([0,1,-1]),drive=rng.choice([0,1,-1]),valid=rng.choice([0,1,-1]),hold=rng.choice([0,1,-1]),phase=rng.choice([-2147483648,-5,-1,0,1,2,3,4,2147483647]),reset_due=1000000000,clock=1000000000+rng.randint(-1,10000000));add(s)
rows=[json.loads(l) for l in (E/'cd-reset-boundaries-baseline-O0-v1.jsonl').read_text().splitlines()[1:]]
for n,row in enumerate(rows[::7]):
 for delta in [-1,0,1,451584]:
  s=row['state'].copy();deadline=s['reset_due'];s['clock']=max(0,min(D['clock'][1],(deadline or s['clock'])+delta));add(s)
with (E/'cd-reset-holdout-v3.json').open('x') as f:json.dump(dict(schema='t172-cd-reset-experiment-v1',cases=cases),f)
print(len(cases))
