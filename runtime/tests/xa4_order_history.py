"""Independent position impulses, conflicting headers, wide history and mode chains."""
import json
import sys
from pathlib import Path
cases=[]
for mode in ('mono','stereo'):
 for group in (0,1,17):
  for block in range(4):
   for nibble in (0,1):
    for sample in (0,13,27):
     writes=[[group*128+16+sample*4+block,7<<(4*nibble)]]
     cases.append(dict(id=f'impulse-{len(cases)}',operations=[dict(op='output_seed',seed=19),dict(op='patch',writes=writes),dict(op=mode)]))
 for header_offset in range(16):
  cases.append(dict(id=f'header-location-{len(cases)}',operations=[dict(op='input_seed',seed=5167),dict(op='patch',writes=[[g*128+i,0] for g in range(18) for i in range(16)]+[[header_offset,0x3c]]),dict(op=mode)]))
 for history in ([0,0,0,0],[1,-1,3,-7],[-32768,32767,32767,-32768],[-8388608,8388607,8388607,-8388608],[8388607]*4,[-8388608]*4):
  for header in (16,32,48,64,255):
   ops=[dict(op='input_seed',seed=718),dict(op='patch',writes=[[g*128+i,header] for g in range(18) for i in range(16)]),dict(op='history',values=history),dict(op=mode),dict(op='stereo' if mode=='mono' else 'mono'),dict(op=mode)]
   cases.append(dict(id=f'history-chain-{len(cases)}',operations=ops))
with Path(sys.argv[1]).open('x') as f:json.dump(dict(schema='t172-xa4-experiment-v1',cases=cases),f,indent=2)
