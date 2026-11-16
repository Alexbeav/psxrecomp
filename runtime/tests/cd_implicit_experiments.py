"""Independent state and numerical boundary fixtures for implicit read seek."""
import itertools,json,random,sys
from pathlib import Path
D=json.loads(Path(sys.argv[2]).read_text())['fields'];base=dict(source_clock=1,drive=1,read_min=0,read_sec=2,read_sect=10,pending=1,setloc=44,last=22,stat=2,mode=0,paused=0,valid=1,lba=7,target=100,hold=0,subq=0,jitter=24999,speed_result=-2147483648,clock=0,due=100);cases=[]
for key,(lo,hi) in D.items():
 for v in sorted(set([lo,hi,0,1,-1])):
  if not lo<=v<=hi:continue
  for source,pending in itertools.product([0,1],[0,1]):
   s=dict(base,source_clock=source,pending=pending);s[key]=v
   if s['hold'] and s['target']>2147483645:continue
   if key=='clock':s['due']=v
   cases.append(dict(id=f'{key}-{v}-{source}-{pending}',state=s))
for mode in range(256):
 for pending in [0,1]:cases.append(dict(id=f'mode-{mode}-{pending}',state=dict(base,mode=mode,pending=pending,clock=100,due=100)))
rng=random.Random(881357)
for n in range(900):
 s={k:rng.choice([lo,hi,0,1]) for k,(lo,hi) in D.items()};s['hold']=rng.choice([0,1,-1]);s['target']=rng.randrange(-2147483648,2147483646);s['lba']=rng.randrange(-150,450000);s['mode']=rng.randrange(256);p=225792 if s['mode']&128 else 451584;s['due']=rng.randrange(1,10**13);steps=rng.choice([0,1,9,4096]);s['clock']=s['due']-1 if not steps else s['due']+(steps-1)*p
 cases.append(dict(id=f'random-{n}',state=s))
with Path(sys.argv[1]).open('x') as f:json.dump(dict(schema='t172-cd-implicit-experiment-v1',cases=cases),f)
print(len(cases))
