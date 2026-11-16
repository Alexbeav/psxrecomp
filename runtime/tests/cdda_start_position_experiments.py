"""Independent boundaries and seeded positions for both input sources."""
import json,random,sys
from pathlib import Path
rng=random.Random(982771)
positions=[0,1,74,75,149,150,449998,449999,2147483648,2147483649,2147483650,3221225471,3221225472,4294967293,4294967294,4294967295]
positions+= [rng.randrange(2147483648,4294967296) for _ in range(160)]
positions+= [rng.randrange(450000) for _ in range(40)]
cases=[dict(id=f'{source}-{n}',source=source,position=p) for source in ('track','current') for n,p in enumerate(positions)]
with Path(sys.argv[1]).open('x') as f:json.dump(dict(schema='t172-cdda-start-position-v1',cases=cases),f,indent=2)
print(len(cases))
