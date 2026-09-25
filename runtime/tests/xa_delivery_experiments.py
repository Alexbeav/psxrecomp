"""Independent routing, classification, coding and stream experiments."""
import json,sys
from pathlib import Path
suite=sys.argv[2];cases=[]
def add(ops):cases.append(dict(id=f'{suite}-{len(cases)}',operations=ops))
def setup(coding=1):return [dict(op='seed',seed=781),dict(op='controls',values=[64,37,91,0]),dict(op='delivery',values=[2,37,91,100,coding,173,211,67])]
if suite in ('coding','mode','submode','rawmode'):
 for v in range(256):
  ops=setup(v if suite=='coding' else 1)
  if suite=='mode':ops[1]['values'][0]=v
  if suite=='submode':ops[2]['values'][3]=v
  if suite=='rawmode':ops[2]['values'][0]=v
  add(ops+[dict(op='call',lba=2147483647)])
elif suite=='stream':
 for active in (-2147483648,-1,0,1,2,2147483647):
  for field in range(4):
   for mismatch in (0,1):
    vals=[37,91,1,active]
    if mismatch:vals[field]=vals[field]^1
    add(setup()+[dict(op='stream',values=vals),dict(op='history',values=[8388607,-8388608,517,-32767]),dict(op='call',lba=-2147483648),dict(op='call',lba=17),dict(op='controls',values=[64,37,91,255]),dict(op='call',lba=18),dict(op='controls',values=[64,37,91,0]),dict(op='call',lba=19)])
elif suite=='filter':
 for file in (0,1,37,91,255):
  for ch in (0,1,37,91,255):
   for coding in (1,16):
    ops=setup(coding);ops[1]['values'][0]=72;ops[2]['values'][1:3]=[file,ch]
    add(ops+[dict(op='call',lba=-1)])
elif suite=='classify':
 for mode in (0,1,2,255):
  for have in (-2147483648,-1,0,1,2,2147483647):
   for null in (0,1):
    add([dict(op='seed',seed=111),dict(op='delivery',values=[255]*8),dict(op='patch',writes=[[15,mode],[16,37],[17,91],[18,100],[19,5],[20,7],[21,8],[22,9],[23,10]]),dict(op='classify',have_raw=have,null_raw=null),dict(op='controls',values=[64,37,91,0]),dict(op='call',lba=23)])
with Path(sys.argv[1]).open('x') as f:json.dump(dict(schema='t172-xa-delivery-experiment-v1',cases=cases),f)
print(suite,len(cases),sum(1+len(c['operations']) for c in cases))
