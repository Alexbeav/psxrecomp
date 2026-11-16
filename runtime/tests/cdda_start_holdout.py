"""Fresh mixed startup chains with seeded state and fatal transitions."""
import json,random,sys
from pathlib import Path
r=random.Random(8053172);domains=json.loads(Path(sys.argv[2]).read_text());cases=[]
for i in range(120):
 ops=[]
 for field,(lo,hi) in domains.items():ops.append(dict(op='set',field=field,value=r.choice([lo,hi,0,r.randint(lo,hi)])))
 ops.append(dict(op='handle',value=r.randrange(3)))
 for j in range(4):
  fix=dict(track_count=r.choice([0,1,3,9,10]),start_lba=r.randrange(450000),selected_track=r.choice([-1,0,1,7,2147483647]),audio=r.choice([-1,0,1]),delay=r.choice([0,r.randrange(100000),2147483647]),jitter=r.randrange(25000),peek_at=r.choice([-1,0,1,15,31]),values=[r.randrange(256) for _ in range(12)])
  if j==0:fix.update(track_count=3,selected_track=1,audio=1,delay=100,peek_at=i%32)
  ops += [dict(op='set',field='reading',value=0 if j<3 else r.choice([0,-1,1])),dict(op='set',field='mode_reg',value=r.randrange(128) if j<3 else r.randrange(256)),dict(op='fixture',**fix),dict(op='start',requested_track=r.choice([-2147483648,-1,0,1,9,2147483647])),dict(op='start',requested_track=0)]
 cases.append(dict(id=f'fresh-{i}',operations=ops))
with Path(sys.argv[1]).open('x') as f:json.dump(dict(schema='t172-cdda-start-experiment-v1',cases=cases),f)
