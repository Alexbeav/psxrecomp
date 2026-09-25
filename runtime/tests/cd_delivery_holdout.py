"""Fresh mixed ownership sequences and complete mode/coordinate edges."""
import json
import random
import sys
from pathlib import Path
def setv(field,value):return dict(op='set',field=field,value=value)
r=random.Random(172092013);cases=[]
for mode in range(256):
 ops=[setv('mode_reg',mode),setv('read_min',65535),setv('read_sec',65535),setv('read_sect',65535)]
 for n in range(4):
  ops += [dict(op='media',delivered=1,have_raw=n%2,user_seed=r.getrandbits(32),raw_seed=r.getrandbits(32)),dict(op='immediate',sequence=r.getrandbits(64)),dict(op='dump',slot=n+1)]
 cases.append(dict(id=f'mode-{mode}',seed=mode,operations=ops))
for n in range(192):
 ops=[]
 for k in range(80):
  choice=r.randrange(12)
  if choice<2:
   size=r.choice([0,1,12,2048,2060,2340,r.randrange(2341)])
   ops.append(dict(op='slot',slot=r.randrange(8),size=size,pos=r.randrange(size+1),seed=r.getrandbits(32)))
  elif choice<4:ops.append(dict(op='media',delivered=r.randrange(2),have_raw=r.randrange(2),user_seed=r.getrandbits(32),raw_seed=r.getrandbits(32),stat=r.randrange(256),end=r.choice([0,1,2147483647])))
  elif choice==4:ops.append(dict(op='end_state',value=r.choice([-2147483648,-1,0,1,2147483647])))
  elif choice<7:
   field,upper=r.choice([('s_ring_read',8),('s_ring_write',8),('mode_reg',256),('stat_reg',256),('irq_flag',8),('s_ring_dropped',2**64),('s_dataready_fires',2**64)])
   ops.append(setv(field,r.randrange(upper)))
  elif choice==7:ops.append(dict(op='response',read=r.randrange(16),count=r.randrange(17),bytes=[r.randrange(256) for _ in range(16)]))
  elif choice==8:ops.append(dict(op='immediate',sequence=r.getrandbits(64)))
  else:ops.append(dict(op=r.choice(['fill','without_irq'])))
 ops += [dict(op='dump',slot=i) for i in range(8)]
 cases.append(dict(id=f'mixed-{n}',seed=r.getrandbits(32),operations=ops))
with Path(sys.argv[1]).open('x') as f:json.dump(dict(schema='t172-cd-delivery-experiment-v1',cases=cases),f,indent=2)
