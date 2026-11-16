"""Full signed sample sweeps plus fresh mixed frame and coefficient observations."""
import json
import random
import sys
from pathlib import Path
cases=[];r=random.Random(172092014)
for coefficients in ([1,255,127,129],[255]*4,[64]*4,[128,0,0,128],[0,128,128,0],[127,129,255,1]):
 for start in range(-32768,32768,8192):
  values=[]
  for v in range(start,start+8192):values.extend([v,-1-v])
  cases.append(dict(id=f'sample-sweep-{len(cases)}',operations=[dict(op='active',values=coefficients),dict(op='samples',values=values),dict(op='apply',frames=8192),dict(op='apply',frames=8192)]))
for n in range(256):
 count=r.choice([0,1,2,3,15,16,17,63,64,65,255]);values=[r.randrange(-32768,32768) for _ in range(count)]
 ops=[dict(op='samples',values=values)]
 for k in range(24):
  choice=r.randrange(4)
  if choice<2:ops.append(dict(op=['active','pending'][choice],values=[r.randrange(256) for _ in range(4)]))
  else:
   frames=r.choice([-2147483648,-65536,-1,0,count//2,r.randrange(count//2+1)])
   ops.append(dict(op='apply',frames=frames,null=r.randrange(2) if frames<=0 else 0))
 cases.append(dict(id=f'mixed-{n}',operations=ops))
with Path(sys.argv[1]).open('x') as f:json.dump(dict(schema='t172-cd-volume-experiment-v1',cases=cases),f,indent=2)
