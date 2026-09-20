"""All command bytes and independent explicit-seek state boundaries."""
import itertools,json,random,sys
from pathlib import Path
D=json.loads(Path(sys.argv[2]).read_text())['fields'];base=dict(source_clock=1,drive=1,read_min=0,read_sec=3,read_sect=10,pending=1,setloc=44,last=22,stat=2,mode=0,paused=1,valid=1,lba=7,target=100,hold=0,subq=0,jitter=71,reading=0,seek_min=0,seek_sec=4,seek_sect=20,command=21,clock=0,due=100);cases=[]
for cmd,source,drive in itertools.product(range(256),[0,1],[0,1]):cases.append(dict(id=f'cmd-{cmd}-{source}-{drive}',state=dict(base,command=cmd,source_clock=source,drive=drive)))
for key,(lo,hi) in D.items():
 for v in sorted(set([lo,hi,0,1,-1])):
  if not lo<=v<=hi:continue
  for command in [0,21,22,255]:
   s=dict(base,command=command);s[key]=v;cases.append(dict(id=f'{key}-{v}-{command}',state=s))
rng=random.Random(306173)
for n in range(900):
 s={k:rng.choice([lo,hi,0,1]) for k,(lo,hi) in D.items()}
 for key in ['stat','mode','paused','command','seek_min','seek_sec','seek_sect']:s[key]=rng.randrange(256)
 s['jitter']=rng.randrange(25000);cases.append(dict(id=f'random-{n}',state=s))
with Path(sys.argv[1]).open('x') as f:json.dump(dict(schema='t172-cd-explicit-experiment-v1',cases=cases),f)
print(len(cases))
