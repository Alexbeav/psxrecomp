"""Independent channel, coefficient, signed rounding, span and NULL experiments."""
import json
import sys
from pathlib import Path
cases=[]
edges=[-32768,-32767,-16385,-16384,-129,-128,-127,-65,-64,-63,-3,-2,-1,0,1,2,3,63,64,65,127,128,129,16383,16384,32766,32767]
samples=[]
for v in edges:samples.extend([v,0,0,v,v,v,v,-v if v!=-32768 else 32767])
samples += [12345]
for slot in range(4):
 for coefficient in range(256):
  active=[0]*4;active[slot]=coefficient
  ops=[dict(op='pending',values=[255,13,89,1]),dict(op='active',values=active),dict(op='samples',values=samples),dict(op='apply',frames=len(samples)//2),dict(op='apply',frames=-2147483648,null=1),dict(op='apply',frames=0),dict(op='apply',frames=1)]
  cases.append(dict(id=f'coefficient-{slot}-{coefficient}',operations=ops))
for matrix in ([128,0,0,128],[0,128,128,0],[64]*4,[255]*4,[127,129,129,127],[1,1,1,1],[255,0,0,255]):
 ops=[dict(op='active',values=matrix),dict(op='samples',values=samples),dict(op='apply',frames=len(samples)//2),dict(op='apply',frames=len(samples)//2),dict(op='reset'),dict(op='apply',frames=0,null=1)]
 cases.append(dict(id=f'matrix-{len(cases)}',operations=ops))
with Path(sys.argv[1]).open('x') as f:json.dump(dict(schema='t172-cd-volume-experiment-v1',cases=cases),f,indent=2)
