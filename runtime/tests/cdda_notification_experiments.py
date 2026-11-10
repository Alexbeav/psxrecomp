"""Notification type/count, overwritten payloads and readiness boundaries."""
import json,sys,itertools
from pathlib import Path
cases=[];suite=sys.argv[2]
def add(ops):cases.append(dict(id=f'{suite}-{len(cases)}',operations=ops))
if suite=='types':
 for t in range(256):
  for n in range(9):add([dict(op='notification',enabled=1,type=4,count=8,values=[211+i for i in range(8)]),dict(op='response',read=13,count=16,values=list(range(64,80))),dict(op='input',values=[(t+i*31)&255 for i in range(8)]),dict(op='queue',type=t,count=n),dict(op='present')])
else:
 for enabled,irq,profile in itertools.product([-2147483648,-1,0,1,2147483647],[0,1,4,7,8,31,128,255],[-2147483648,-1,0,1,2147483647]):
  for clock,due in [(0,0),(0,1),(1,0),(2**64-2,2**64-1),(2**64-1,2**64-1)]:
   add([dict(op='notification',enabled=enabled,type=4,count=1,values=[51]*8),dict(op='external',irq=irq,source_clock=profile,clock=clock,ready_due=due),dict(op='present'),dict(op='input',values=[17]*8),dict(op='queue',type=1,count=8),dict(op='queue',type=4,count=1),dict(op='external',irq=0,source_clock=profile,clock=due,ready_due=due),dict(op='present'),dict(op='present')])
with Path(sys.argv[1]).open('x') as f:json.dump(dict(schema='t172-cdda-notification-experiment-v1',cases=cases),f)
print(suite,len(cases),sum(len(c['operations'])+1 for c in cases))
