"""Change readiness after scheduling and probe repeated release boundaries."""
import json
import sys
from pathlib import Path
from cd_pending_experiments import setv
cases=[]
for profile in (0,1):
 for irq in range(8):
  for ready in (0,499,500,501,1999,2000,2001,5000):
   for delivered in (0,1):
    ops=[dict(op='arrive',delivered=1,sequence=19),setv('s_source_clock',profile),dict(op='schedule'),setv('s_source_ready_due',ready),setv('irq_flag',irq),dict(op='arrive',delivered=delivered,sequence=20)]
    for clock in (499,500,501,1999,2000,2001,4999,5000,5001):
     ops += [setv('psx_cycle_count',clock),dict(op='service')]
    ops += [setv('irq_flag',0),dict(op='schedule'),setv('psx_cycle_count',7001),dict(op='service'),dict(op='present'),dict(op='clear')]
    cases.append(dict(id=f'deadline-{len(cases)}',operations=ops))
with Path(sys.argv[1]).open('x') as f:json.dump(dict(schema='t172-cd-pending-experiment-v1',cases=cases),f,indent=2)
