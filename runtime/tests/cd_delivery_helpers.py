"""Helper entry, media result overrides, response storage and counter boundaries."""
import json
import sys
from pathlib import Path
def setv(field,value):return dict(op='set',field=field,value=value)
cases=[]
for method in ('immediate','without_irq'):
 for end in (-2147483648,-1,0,1,2147483647):
  for delivered in (0,1):
   for irq in (0,1,7):
    for override in (None,0,1,2147483647):
     n=len(cases);media=dict(op='media',delivered=delivered,have_raw=n%2,user_seed=n+51,raw_seed=n+119,stat=(n*37)%256)
     if override is not None:media['end']=override
     call=dict(op=method)
     if method=='immediate':call['sequence']=[0,1,2**64-1][n%3]
     ops=[setv('s_ring_read',n%8),setv('s_ring_write',(n+1)%8),setv('read_min',n%65536),setv('read_sec',59),setv('read_sect',74),setv('mode_reg',[0,32,255][n%3]),setv('stat_reg',193),setv('irq_flag',irq),setv('s_ring_dropped',2**64-1),setv('s_dataready_fires',2**64-1),dict(op='end_state',value=end),dict(op='response',read=7,count=16,bytes=list(range(16))),media,call,call]+[dict(op='dump',slot=i) for i in range(8)]
     cases.append(dict(id=f'helpers-{n}',seed=n+7,operations=ops))
with Path(sys.argv[1]).open('x') as f:json.dump(dict(schema='t172-cd-delivery-experiment-v1',cases=cases),f,indent=2)
