"""Independent header/status/position and omitted-write observations."""
import itertools,json,sys
from pathlib import Path
suite=sys.argv[2];cases=[]
def add(ret,valid,head,lba,write_valid=1,write_bytes=1):
 cases.append(dict(id=f'{suite}-{len(cases)}',operations=[dict(op='stored',available=-17,position_valid=23,values=list(range(32,44))),dict(op='handle',value=len(cases)%3),dict(op='reader',**{'return':ret},valid=valid,write_valid=write_valid,write_bytes=write_bytes,values=[head]+[(i*31+head)&255 for i in range(11)]),dict(op='peek',lba=lba),dict(op='peek',lba=-lba-1)]))
if suite=='headers':
 for head in range(256):add(1,1,head,172)
else:
 for ret,valid,lba in itertools.product([-2147483648,-1,0,1,2147483647],[-2147483648,-1,0,1,2147483647],[-2147483648,-151,-150,-1,0,1,2147483647]):
  add(ret,valid,1,lba)
  add(ret,valid,1,lba,0,0)
  if ret==0 or valid==0:add(ret,valid,1,lba,1,0)
with Path(sys.argv[1]).open('x') as f:json.dump(dict(schema='t172-cdda-peek-experiment-v1',cases=cases),f)
print(suite,len(cases))
