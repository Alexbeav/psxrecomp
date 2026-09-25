"""Fresh success/failure chains and prior-position preservation."""
import json,random,sys
from pathlib import Path
r=random.Random(24172);cases=[]
for i in range(100):
 ops=[dict(op='stored',available=r.randint(-2147483648,2147483647),position_valid=r.randint(-2147483648,2147483647),values=[r.randrange(256) for _ in range(12)])]
 for j in range(12):
  ret=r.choice([0,1,-1,r.randint(-2147483648,2147483647)]);valid=r.choice([0,1,-1,r.randint(-2147483648,2147483647)]);wv=r.randrange(2);wb=1 if ret and valid and wv else r.randrange(2)
  payload=[r.randrange(256) for _ in range(12)]
  if j%3==0:payload[0]=(payload[0]&240)|1
  ops += [dict(op='handle',value=r.randrange(3)),dict(op='reader',**{'return':ret},valid=valid,write_valid=wv,write_bytes=wb,values=payload),dict(op='peek',lba=r.randint(-2147483648,2147483647))]
 cases.append(dict(id=f'fresh-{i}',operations=ops))
with Path(sys.argv[1]).open('x') as f:json.dump(dict(schema='t172-cdda-peek-experiment-v1',cases=cases),f)
