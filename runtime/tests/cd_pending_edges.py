"""Boundary observations selected before implementation."""
import json
import sys
from pathlib import Path
from cd_pending_experiments import setv
cases=[]
for profile in (0,1):
 for irq in (0,1,7):
  for now in (0,1000,2**64-2001):
   for ready in (0,999,1000,1001,2999,3000,3001,2**64-2001):
    setup=[setv('s_source_clock',profile),setv('irq_flag',irq),setv('psx_cycle_count',now),setv('s_source_ready_due',ready),setv('stat_reg',213),setv('s_ring_write',6),setv('last_sector_lba',123456),dict(op='arrive',delivered=1,sequence=2**64-1)]
    tail=[dict(op='schedule'),dict(op='service'),setv('psx_cycle_count',2**64-2001),dict(op='service'),setv('irq_flag',0),dict(op='schedule'),dict(op='arrive',delivered=0,sequence=4),dict(op='arrive',delivered=1,sequence=0),dict(op='clear'),dict(op='schedule'),dict(op='service')]
    cases.append(dict(id=f'edge-{len(cases)}',operations=setup+tail))
with Path(sys.argv[1]).open('x') as f:json.dump(dict(schema='t172-cd-pending-experiment-v1',cases=cases),f,indent=2)
