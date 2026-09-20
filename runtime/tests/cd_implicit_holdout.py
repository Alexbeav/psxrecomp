"""Fresh chains, jitter saturation and accepted seek transition boundaries."""
import itertools,json,random,sys
from pathlib import Path
rng=random.Random(132770);prior=[json.loads(l) for l in Path(sys.argv[1]).read_text().splitlines()[1:]];cases=[]
for row in rng.sample(prior,250):
 s=row['state'].copy()
 if not -150<=s['lba']<=449999:continue
 s['mode']=rng.randrange(256);period=225792 if s['mode']&128 else 451584;s['due']=rng.randrange(1,10**12);s['clock']=s['due']+rng.choice([-1,0,1,period*4095]);s['source_clock']=rng.choice([0,1,-1]);s['pending']=rng.choice([0,1,-1]);s['jitter']=rng.randrange(25000);s['speed_result']=rng.randrange(-2147483648,2147483648)
 cases.append(dict(id='chain-'+row['case_id'],state=s))
base=dict(source_clock=1,drive=1,read_min=0,read_sec=2,read_sect=0,pending=1,setloc=0,last=0,stat=2,mode=0,paused=0,valid=1,lba=0,target=0,hold=0,subq=0,jitter=0,speed_result=987654,clock=0,due=1)
for source,drive,mode,paused,distance,jitter in itertools.product([0,1],[0,1],[0,128],[0,1],[0,2,3,11,12,2249,2250,20446369,2147483647],[0,1,24999]):
 s=dict(base,source_clock=source,drive=drive,mode=mode,paused=paused,setloc=distance,jitter=jitter);cases.append(dict(id='edge-'+str(len(cases)),state=s))
for n in range(700):
 s=base.copy();s.update(source_clock=rng.choice([0,1,-1]),drive=rng.choice([0,1,-1]),read_min=rng.randrange(-65535,65536),read_sec=rng.randrange(-65535,65536),read_sect=rng.randrange(-65535,65536),pending=rng.choice([0,1,-1]),setloc=rng.randrange(-2147483648,2147483648),last=rng.randrange(-2147483648,2147483648),stat=rng.randrange(256),mode=rng.randrange(256),paused=rng.randrange(256),valid=rng.choice([0,1,-1]),lba=rng.randrange(-150,450000),target=rng.randrange(-2147483648,2147483646),hold=rng.choice([0,1,-1]),subq=rng.randrange(-2147483648,2147483648),jitter=rng.randrange(25000),speed_result=rng.randrange(-2147483648,2147483648))
 s['due']=rng.randrange(1,10**12);p=225792 if s['mode']&128 else 451584;s['clock']=s['due']+rng.choice([-1,0,p*4095]);cases.append(dict(id='fresh-'+str(n),state=s))
with Path(sys.argv[2]).open('x') as f:json.dump(dict(schema='t172-cd-implicit-experiment-v1',cases=cases),f)
print(len(cases))
