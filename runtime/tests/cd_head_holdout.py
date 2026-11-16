"""Fresh boundaries and chains seeded from prior opaque exit states."""
import json,random,sys
from pathlib import Path
rng=random.Random(569131);cases=[]
prior=[json.loads(l) for l in Path(sys.argv[1]).read_text().splitlines()[1:]]
for n,row in enumerate(rng.sample(prior,300)):
 s=row['state'].copy()
 if not -150<=s['lba']<=449999:continue
 s['mode']=rng.randrange(256);period=225792 if s['mode']&128 else 451584
 if s['due']>2**64-451585-period*4096:continue
 s['clock']=max(0,s['due']+rng.choice([-1,0,1,period-1,period,period*4095]));s['hold']=rng.choice([0,1,-1]);s['drive']=1;s['valid']=1
 cases.append(dict(id='chain-'+row['case_id'],state=s))
for n in range(750):
 mode=rng.randrange(256);period=225792 if mode&128 else 451584;target=rng.choice([-150,-149,-145,-142,-141,-1,0,1,449990,449999]);lba=max(-150,min(449999,target+rng.randrange(-12,13)));due=rng.randrange(1,1000000);steps=rng.choice([0,1,8,9,10,4096])
 s=dict(drive=rng.choice([0,1,-1,2147483647]),valid=rng.choice([0,1,-1,-2147483648]),hold=rng.choice([0,1,-1,2147483647]),lba=lba,target=target,subq=rng.randrange(-2147483648,2147483648),mode=mode,due=due,clock=due-1 if not steps else due+(steps-1)*period)
 cases.append(dict(id='fresh-'+str(n),state=s))
with Path(sys.argv[2]).open('x') as f:json.dump(dict(schema='t172-cd-head-experiment-v1',cases=cases),f)
print(len(cases))
