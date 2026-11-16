"""Fresh TOC position-selection, saturation and sentinel-preservation holdouts."""
import itertools,json,random,sys
from pathlib import Path
rng=random.Random(605278);prior=[json.loads(l) for l in Path(sys.argv[1]).read_text().splitlines()[1:]];cases=[]
for row in rng.sample(prior,200):
 s=row['state'].copy();s['source_clock']=rng.choice([0,1,-1]);s['reading']=rng.choice([0,1,-1]);s['paused']=rng.choice([0,1,255]);s['jitter']=rng.randrange(25000);s['clock']=rng.randrange(2**64);s['due']=rng.randrange(2**64)
 cases.append(dict(id='chain-'+row['case_id'],state=s))
base=prior[0]['state']
for source,reading,paused,drive,valid,stat in itertools.product([0,1,-1],[0,1,-1],[0,1,255],[0,1],[0,1],[0,2,34,66,130,255]):
 s=dict(base,source_clock=source,reading=reading,paused=paused,drive=drive,valid=valid,stat=stat,last=2147483647,lba=-2147483648,read_min=-65535,read_sec=65535,read_sect=-65535,clock=2**64-1,due=2**64-1,jitter=24999)
 cases.append(dict(id='selection-'+str(len(cases)),state=s))
for n in range(900):
 s={k:rng.randrange(-2147483648,2147483648) for k in base};s.update(read_min=rng.randrange(-65535,65536),read_sec=rng.randrange(-65535,65536),read_sect=rng.randrange(-65535,65536),stat=rng.randrange(256),mode=rng.randrange(256),paused=rng.randrange(256),source_clock=rng.choice([0,1,-1]),reading=rng.choice([0,1,-1]),valid=rng.choice([0,1,-1]),drive=rng.choice([0,1,-1]),clock=rng.randrange(2**64),due=rng.randrange(2**64),jitter=rng.randrange(25000))
 cases.append(dict(id='random-'+str(n),state=s))
with Path(sys.argv[2]).open('x') as f:json.dump(dict(schema='t172-cd-toc-experiment-v1',cases=cases),f)
print(len(cases))
