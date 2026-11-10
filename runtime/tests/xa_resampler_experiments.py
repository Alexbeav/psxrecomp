"""Synthetic count, capacity, null and overflow-transition experiments."""
import json,random,sys
from pathlib import Path
r=random.Random(17220260920);cases=[]
def add(n,rate,cap,samples=None,**kw):
 if samples is None:samples=[r.randint(-32768,32767) for _ in range(2*max(n,0))]
 cases.append(dict(id=f'case-{len(cases)}',samples=samples,in_frames=n,sample_rate=rate,max_frames=cap,output_seed=r.getrandbits(32),**kw))
for n in (-2147483648,-1,0,1,2,5,6,7,13,31,2016,4032):
 for rate in (18900,37800):
  full=(max(n,0)*44100+rate-1)//rate
  for cap in sorted(set([-2147483648,-1,0,1,max(0,full-1),min(9408,full),min(9408,full+1),9408])):add(n,rate,cap)
for null_in,null_out in ((1,0),(0,1),(1,1)):
 for n in (-2147483648,0,1,4032):
  for rate in (-2147483648,-1,0,18900,37800):add(n,rate,9408,[],null_in=null_in,null_out=null_out)
for rate in (-2147483648,-1,0):add(4032,rate,9408)
values=(-32768,-32767,-16385,-1,0,1,16384,32766,32767)
for a in values:
 for b in values:
  for rate in (18900,37800):add(8,rate,9408,[v for i in range(8) for v in ((a,b) if i%2 else (b,a))])
for rate in (18900,37800):
 for n in (2016,4032):
  for phase in range(7):add(n,rate,9408,[v for i in range(n) for v in ((32767,-32768) if (i+phase)%7<3 else (-32768,32767))])
with Path(sys.argv[1]).open('x') as f:json.dump(dict(schema='t172-xa-resampler-experiment-v1',cases=cases),f)
print(len(cases))
