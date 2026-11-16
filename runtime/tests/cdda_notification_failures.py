"""Independent oversized-count vectors, including unsigned boundaries."""
import json,random,sys
from pathlib import Path
r=random.Random(525172);cases=[]
counts=[9,10,15,16,17,31,32,255,256,65535,65536,2147483647,2147483648,4294967294,4294967295]
for typ in (0,1,4,7,128,255):
 for count in counts:cases.append(dict(id=f'boundary-{typ}-{count}',type=typ,count=count,payload=[r.randrange(256) for _ in range(8)]))
for i in range(70):cases.append(dict(id=f'mixed-{i}',type=r.randrange(256),count=r.randint(9,4294967295),payload=[r.randrange(256) for _ in range(8)]))
with Path(sys.argv[1]).open('x') as f:json.dump(dict(schema='t172-cdda-notification-failure-v1',cases=cases),f)
