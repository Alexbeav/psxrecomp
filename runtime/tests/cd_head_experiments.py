"""Bounded states across flags, target offsets, clock edges and speed modes."""
import itertools,json,random,sys
from pathlib import Path
rng=random.Random(340127);cases=[]
def add(s):cases.append(dict(id=str(len(cases)),state=s))
for drive,valid,hold,mode in itertools.product([0,1,-1,-2147483648,2147483647],[0,1,-1],[0,1,-1],[0,128,255]):
 for target,delta,steps in [(-150,0,1),(0,-1,1),(0,0,2),(100,-2,1),(100,-1,1),(100,0,1),(100,1,1),(449990,9,4096)]:
  period=225792 if mode&128 else 451584
  add(dict(drive=drive,valid=valid,hold=hold,lba=target+delta,target=target,subq=-2147483648,mode=mode,due=0,clock=(steps-1)*period))
for mode in range(256):
 for gap in [-1,0,1]:add(dict(drive=1,valid=1,hold=mode%3-1,lba=100,target=100,subq=2147483647,mode=mode,due=100,clock=100+gap))
for i in range(1700):
 mode=rng.randrange(256);period=225792 if mode&128 else 451584;n=rng.choice([0,1,2,8,9,10,4095,4096]);due=rng.randrange(1,2**64-451584-4096*period)
 add(dict(drive=rng.choice([0,1,-1]),valid=rng.choice([0,1,-1]),hold=rng.choice([0,1,-1,-2147483648]),lba=rng.randrange(-150,450000),target=rng.randrange(-150,450000),subq=rng.randrange(-2147483648,2147483648),mode=mode,due=due,clock=due-1 if not n else due+(n-1)*period+rng.randrange(period)))
# near upper clock limit, exact one-step deadlines
for mode in [0,128]:
 for hold in [0,1]:add(dict(drive=1,valid=1,hold=hold,lba=449999,target=449999,subq=0,mode=mode,due=2**64-1-451584,clock=2**64-1-451584))
with Path(sys.argv[1]).open('x') as f:json.dump(dict(schema='t172-cd-head-experiment-v1',cases=cases),f)
print(len(cases))
