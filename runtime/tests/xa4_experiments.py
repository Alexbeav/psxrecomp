"""Independent full-header-byte, signed nibble and channel-history observations."""
import json
import sys
from pathlib import Path
mode=sys.argv[2];cases=[]
for header in range(256):
    writes=[[g*128+i,header] for g in range(18) for i in range(16)]
    ops=[dict(op='input_seed',seed=17209),dict(op='output_seed',seed=729),dict(op='patch',writes=writes),dict(op='history',values=[1337,-2311,32767,-32768]),dict(op=mode)]
    cases.append(dict(id=f'{mode}-header-{header}',operations=ops))
for nibble in range(16):
    writes=[[g*128+i,nibble*17] for g in range(18) for i in range(16,128)]
    cases.append(dict(id=f'{mode}-nibble-{nibble}',operations=[dict(op='patch',writes=writes),dict(op=mode)]))
with Path(sys.argv[1]).open('x') as f:json.dump(dict(schema='t172-xa4-experiment-v1',cases=cases),f,indent=2)
