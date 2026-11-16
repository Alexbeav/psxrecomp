"""Fresh chained sectors with changing filter, mute, coding and volume."""
import json,random,sys
from pathlib import Path
r=random.Random(986172);cases=[]
for i in range(40):
 ops=[dict(op='history',values=[r.randint(-8388608,8388607) for _ in range(4)]),dict(op='stream',values=[r.randrange(256),r.randrange(256),r.randrange(256),r.choice([-2147483648,-1,0,1,2147483647])])]
 file=r.randrange(256);ch=r.randrange(256)
 for j in range(4):
  coding=r.choice([0,1,2,3,4,5,6,7,64,65,128,129,192,193,8,16,32,255]);mode=r.choice([64,72,255,0]);mute=r.choice([0,0,0,1,255]);sub=r.choice([68,100,196,228,255,0]);dm=r.choice([2,2,2,1])
  ops += [dict(op='seed',seed=r.getrandbits(32)),dict(op='patch',writes=[[15,dm],[16,file],[17,ch],[18,sub],[19,coding],[20,file^255],[21,ch^255],[22,sub^255],[23,coding^255]]),dict(op='classify',have_raw=r.choice([1,1,1,-1,0]),null_raw=0),dict(op='controls',values=[mode,file if j%3 else file^1,ch,mute]),dict(op='active',values=[r.randrange(256) for _ in range(4)]),dict(op='pending',values=[r.randrange(256) for _ in range(4)]),dict(op='call',lba=r.randint(-2147483648,2147483647),null_raw=int(i%17==0),null_delivery=int(i%19==0))]
  if j==1:ops += [dict(op='controls',values=[64,file,ch,0]),dict(op='delivery',values=[2,file,ch,100,coding&199,255,255,255]),dict(op='call',lba=-172)]
 cases.append(dict(id=f'fresh-{i}',operations=ops))
with Path(sys.argv[1]).open('x') as f:json.dump(dict(schema='t172-xa-delivery-experiment-v1',cases=cases),f)
