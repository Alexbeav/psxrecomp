"""Independent signed target extremes including inactive and future-deadline calls."""
import itertools,json,random,sys
from pathlib import Path
cases=[]
for target,hold,mode,lba,steps in itertools.product([-2147483648,-2147483647,-151,-150,0,449999,2147483644,2147483645,2147483646,2147483647],[0,1,-1],[0,128],[-150,0,449999],[0,1,2,4096]):
 if hold and target>2147483645:continue
 period=225792 if mode&128 else 451584
 cases.append(dict(id='edge-'+str(len(cases)),state=dict(drive=1,valid=1,hold=hold,lba=lba,target=target,subq=-2147483648,mode=mode,due=71,clock=70 if steps==0 else 71+(steps-1)*period)))
for drive,valid,hold,target in itertools.product([0,1,-1],[0,1,-1],[0,1],[-2147483648,2147483645,2147483647]):
 if hold and target>2147483645:continue
 cases.append(dict(id='inactive-'+str(len(cases)),state=dict(drive=drive,valid=valid,hold=hold,lba=17,target=target,subq=2147483647,mode=255,due=200,clock=199)))
rng=random.Random(5550219)
for n in range(600):
 hold=rng.choice([0,1,-1]);mode=rng.randrange(256);period=225792 if mode&128 else 451584;due=rng.randrange(1,10**12);steps=rng.choice([0,1,9,4096]);target=rng.randrange(-2147483648,2147483646 if hold else 2147483648)
 cases.append(dict(id='random-'+str(n),state=dict(drive=1,valid=rng.choice([0,1,-1]),hold=hold,lba=rng.randrange(-150,450000),target=target,subq=rng.randrange(-2147483648,2147483648),mode=mode,due=due,clock=due-1 if not steps else due+(steps-1)*period)))
with Path(sys.argv[1]).open('x') as f:json.dump(dict(schema='t172-cd-head-experiment-v1',cases=cases),f)
print(len(cases))
