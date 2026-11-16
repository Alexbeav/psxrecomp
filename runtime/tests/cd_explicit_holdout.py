"""Fresh explicit seek command/mode/reading interactions and chained state."""
import itertools,json,random,sys
from pathlib import Path
rng=random.Random(7771409);prior=[json.loads(l) for l in Path(sys.argv[1]).read_text().splitlines()[1:]];cases=[]
for row in rng.sample(prior,200):
 s=row['state'].copy();s['command']=rng.randrange(256);s['source_clock']=rng.choice([0,1,-1]);s['reading']=rng.choice([0,1,-1]);s['jitter']=rng.randrange(25000);s['clock']=rng.randrange(2**64);s['due']=rng.randrange(2**64);cases.append(dict(id='chain-'+row['case_id'],state=s))
base=prior[0]['state']
for mode,cmd,drive,reading in itertools.product(range(256),[21,22],[0,1],[0,1]):
 s=dict(base,source_clock=mode%2,mode=mode,command=cmd,drive=drive,reading=reading,valid=1,lba=rng.choice([-2147483648,-150,0,2147483647]),read_min=rng.choice([-65535,0,65535]),read_sec=rng.choice([-65535,0,65535]),read_sect=rng.choice([-65535,0,65535]),seek_min=rng.choice([0,255]),seek_sec=rng.choice([0,255]),seek_sect=rng.choice([0,255]),jitter=rng.choice([0,24999]),clock=2**64-1,due=2**64-1)
 cases.append(dict(id='mode-'+str(len(cases)),state=s))
for n in range(700):
 s={k:rng.randrange(-2147483648,2147483648) for k in base}
 for k in ['stat','mode','paused','command','seek_min','seek_sec','seek_sect']:s[k]=rng.randrange(256)
 for k in ['read_min','read_sec','read_sect']:s[k]=rng.randrange(-65535,65536)
 for k in ['source_clock','drive','valid','reading']:s[k]=rng.choice([0,1,-1])
 s.update(jitter=rng.randrange(25000),clock=rng.randrange(2**64),due=rng.randrange(2**64));cases.append(dict(id='random-'+str(n),state=s))
with Path(sys.argv[2]).open('x') as f:json.dump(dict(schema='t172-cd-explicit-experiment-v1',cases=cases),f)
print(len(cases))
