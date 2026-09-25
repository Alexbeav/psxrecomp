"""Fresh stateful mixtures after production implementation."""
import json,random,sys
from pathlib import Path
r=random.Random(2172172);cases=[]
for i in range(150):
 ops=[]
 for j in range(60):
  k=r.choice(['queue','queue','present','present','input','notification','external','response','reset'])
  op=dict(op=k)
  if k=='queue':op.update(type=r.randrange(256),count=r.randrange(9))
  if k=='input':op.update(values=[r.randrange(256) for _ in range(8)])
  if k=='notification':op.update(enabled=r.choice([0,-1,1,r.randint(-2147483648,2147483647)]),type=r.randrange(256),count=r.randrange(9),values=[r.randrange(256) for _ in range(8)])
  if k=='external':op.update(irq=r.choice([0,0,r.randrange(256)]),source_clock=r.choice([0,1,-1]),clock=r.getrandbits(64),ready_due=r.choice([0,r.getrandbits(64),2**64-1]))
  if k=='response':op.update(read=r.randrange(16),count=r.randrange(17),values=[r.randrange(256) for _ in range(16)])
  ops.append(op)
 cases.append(dict(id=f'fresh-{i}',operations=ops))
with Path(sys.argv[1]).open('x') as f:json.dump(dict(schema='t172-cdda-notification-experiment-v1',cases=cases),f)
