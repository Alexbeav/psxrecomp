"""Independent synthetic state seeding; this does not invoke a snapshot codec."""
import json
import random
import sys
from pathlib import Path
from cd_pending_experiments import setv
r=random.Random(172092012);cases=[]
for profile in (0,1):
 for pending in (0,1):
  for irq in range(8):
   for due in (0,1,499,500,501,2**64-2001):
    for first in ('clear','present','schedule','service'):
     ops=[setv('s_source_clock',profile),setv('irq_flag',irq),setv('psx_cycle_count',500),setv('s_source_ready_due',1000),dict(op='response',read=15,count=16,bytes=list(range(16))),dict(op='notification_state',pending=pending,stat=r.randrange(256),slot=r.randrange(8),due=due,sequence=r.getrandbits(64)),dict(op=first),dict(op='service'),setv('psx_cycle_count',1000),dict(op='service'),setv('irq_flag',0),dict(op='schedule'),setv('psx_cycle_count',3000),dict(op='service'),dict(op='present')]
     cases.append(dict(id=f'seeded-{len(cases)}',operations=ops))
for n in range(256):
 ops=[]
 for step in range(128):
  choice=r.randrange(10)
  if choice<2:ops.append(dict(op='notification_state',pending=r.randrange(2),stat=r.randrange(256),slot=r.randrange(8),due=r.choice([0,1,499,500,501,2000,2**64-2001]),sequence=r.getrandbits(64)))
  elif choice<5:
   f,values=r.choice([('irq_flag',range(8)),('s_source_clock',range(2)),('stat_reg',range(256)),('s_ring_write',range(8)),('psx_cycle_count',[0,1,500,2000,2**64-2001]),('s_source_ready_due',[0,1,501,2001,2**64-2001])]);ops.append(setv(f,r.choice(values)))
  elif choice<7:ops.append(dict(op='arrive',delivered=r.randrange(2),sequence=r.getrandbits(64)))
  else:ops.append(dict(op=r.choice(['clear','present','schedule','service'])))
 cases.append(dict(id=f'seeded-mixed-{n}',operations=ops))
with Path(sys.argv[1]).open('x') as f:json.dump(dict(schema='t172-cd-pending-experiment-v1',cases=cases),f,indent=2)
