"""Independent synthetic ring ownership, source selection and delivery experiments."""
import json
import sys
from pathlib import Path
def setv(field,value):return dict(op='set',field=field,value=value)
cases=[]
for read in range(8):
 for write in range(8):
  for delivered in (0,1):
   for raw in (0,1):
    for mode in (0,16,32,255):
     ops=[setv('s_ring_read',read),setv('s_ring_write',write),setv('mode_reg',mode),setv('read_min',12),setv('read_sec',34),setv('read_sect',56),dict(op='media',delivered=delivered,have_raw=raw,user_seed=137,raw_seed=981)]
     for i in range(8):ops.append(dict(op='slot',slot=i,size=[0,17,2048,2340][i%4],pos=[0,0,1024,2340][i%4],seed=17+i))
     ops+=[dict(op='fill')]+[dict(op='dump',slot=i) for i in range(8)]
     cases.append(dict(id=f'fill-{len(cases)}',seed=9,operations=ops))
with Path(sys.argv[1]).open('x') as f:json.dump(dict(schema='t172-cd-delivery-experiment-v1',cases=cases),f,indent=2)
