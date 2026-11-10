"""Fresh mixed full-range samples after production commit."""
import json,random,sys
from pathlib import Path
r=random.Random(920172);cases=[]
for i in range(300):
 n=r.randint(1,4032);rate=r.choice([18900,37800]);full=(n*44100+rate-1)//rate
 samples=[r.randint(-32768,32767) for _ in range(n*2)]
 cases.append(dict(id=f'fresh-{i}',samples=samples,in_frames=n,sample_rate=rate,max_frames=r.choice([0,1,max(0,full-1),full,min(9408,full+1),9408,r.randint(0,9408)]),output_seed=r.getrandbits(32)))
with Path(sys.argv[1]).open('x') as f:json.dump(dict(schema='t172-xa-resampler-experiment-v1',cases=cases),f)
