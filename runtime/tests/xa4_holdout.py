import json, random
from pathlib import Path
r=random.Random(20260920172)
cases=[]
for i in range(160):
    ops=[dict(op='input_seed',seed=r.getrandbits(32)),dict(op='output_seed',seed=r.getrandbits(32)),dict(op='history',values=[r.randint(-8388608,8388607) for _ in range(4)])]
    for j in range(3):
        ops.append(dict(op='patch',writes=[[g*128+k,r.randrange(256)] for g in range(18) for k in range(16)]+[[r.randrange(2304),r.randrange(256)] for _ in range(32)]))
        ops.append(dict(op=('mono' if (i+j)%3 else 'stereo')))
    cases.append(dict(id=f'mixed-{i}',operations=ops))
import sys
with Path(sys.argv[1]).open('x') as f:json.dump(dict(schema='t172-xa4-experiment-v1',cases=cases),f)
