"""Fresh mixed sequences and full byte/ring sweeps, generated after implementation."""
import json
import random
import sys
from pathlib import Path
from cd_pending_experiments import setv
r=random.Random(172092011)
cases=[]
for status in range(256):
 ops=[dict(op='response',read=status%16,count=status%17,bytes=[(status+i*37)%256 for i in range(16)])]
 for slot in range(8):
  clock=r.randrange(2**48)
  ops += [setv('psx_cycle_count',clock),setv('s_source_clock',slot%2),setv('irq_flag',slot),setv('s_ring_write',slot),setv('s_ring_read',7-slot),setv('last_sector_lba',r.randrange(450000)),setv('stat_reg',status),dict(op='arrive',delivered=1,sequence=r.getrandbits(64)),dict(op='schedule'),setv('irq_flag',0),dict(op='schedule'),setv('psx_cycle_count',clock+500),dict(op='service'),setv('psx_cycle_count',clock+2000),dict(op='service'),dict(op='present')]
 cases.append(dict(id=f'byte-ring-{status}',operations=ops))
for case in range(512):
 ops=[]; clock=r.randrange(2**40)
 for step in range(128):
  choice=r.randrange(15)
  if choice<5:
   f,upper=r.choice([('irq_flag',8),('stat_reg',256),('s_ring_read',8),('s_ring_write',8),('last_sector_lba',450000),('s_source_clock',2)])
   ops.append(setv(f,r.randrange(upper)))
  elif choice==5:
   clock+=r.choice([0,1,499,500,501,1999,2000,2001]);ops.append(setv('psx_cycle_count',clock))
  elif choice==6:ops.append(setv('s_source_ready_due',clock+r.randrange(4000)))
  elif choice<10:ops.append(dict(op='arrive',delivered=int(choice!=7),sequence=r.getrandbits(64)))
  else:ops.append(dict(op=r.choice(['clear','present','schedule','service'])))
 cases.append(dict(id=f'mixed-holdout-{case}',operations=ops))
with Path(sys.argv[1]).open('x') as f:json.dump(dict(schema='t172-cd-pending-experiment-v1',cases=cases),f,indent=2)
