"""Independent TOC seek field and status-bit boundaries."""
import itertools,json,random,sys
from pathlib import Path
D=json.loads(Path(sys.argv[2]).read_text())['fields'];base=dict(source_clock=1,drive=1,read_min=0,read_sec=3,read_sect=10,pending=1,setloc=44,last=22,stat=2,mode=0,paused=0,valid=1,lba=7,target=100,hold=0,subq=0,jitter=24999,reading=0,clock=0,due=100);cases=[]
for k,(lo,hi) in D.items():
 for v in sorted(set([lo,hi,0,1,-1])):
  if not lo<=v<=hi:continue
  for drive,valid in itertools.product([0,1],[0,1]):cases.append(dict(id=f'{k}-{v}-{drive}-{valid}',state=dict(base,**dict(drive=drive,valid=valid,**{k:v})) if k not in ['drive','valid'] else dict(dict(base,drive=drive,valid=valid),**{k:v})))
for stat,reading,mode in itertools.product(range(256),[0,1,-1],[0,128]):cases.append(dict(id=f'status-{stat}-{reading}-{mode}',state=dict(base,stat=stat,reading=reading,mode=mode)))
for mode in range(256):cases.append(dict(id=f'mode-{mode}',state=dict(base,mode=mode)))
rng=random.Random(507119)
for n in range(800):
 s={k:rng.choice([lo,hi,0,1]) for k,(lo,hi) in D.items()};s['stat']=rng.randrange(256);s['mode']=rng.randrange(256);s['paused']=rng.randrange(256);s['jitter']=rng.randrange(25000)
 cases.append(dict(id=f'random-{n}',state=s))
with Path(sys.argv[1]).open('x') as f:json.dump(dict(schema='t172-cd-toc-experiment-v1',cases=cases),f)
print(len(cases))
