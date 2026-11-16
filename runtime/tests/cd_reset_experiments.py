"""Independent reset boundaries, deadline arithmetic and sentinel preservation."""
import itertools,json,random,sys
from pathlib import Path
D=json.loads(Path(sys.argv[2]).read_text())['fields'];s={k:0 for k in D};s.update(source_clock=1,drive=1,read_min=9,read_sec=8,read_sect=7,pending=1,setloc=333,last=222,stat=255,mode=255,paused=1,valid=1,lba=444,target=555,hold=1,subq=666,reading=1,seek_min=5,seek_sec=6,seek_sect=7,muted=1,far=1,ready=1,clock=1000,due=1200,reset_due=1000)
cases=[]
def add(state):
 if state['hold']:state['target']=min(state['target'],2147483645)
 if state['source_clock'] and state['drive'] and state['reset_due'] and state['clock']>=state['reset_due']:
  state['clock']=min(state['clock'],state['reset_due']+4095*225792)
 cases.append(dict(id=str(len(cases)),state=state))
for p,drive,mode,elapsed in itertools.product([-2147483648,-4000,-4,-1,0,1,2,3,4,2147483647],[0,1],[0,128],[0,225791,225792,451583,451584,451585,903168,1354752,4095*225792]):add(dict(s,phase=p,drive=drive,mode=mode,clock=1000+elapsed))
for key,(lo,hi) in D.items():
 for value in sorted(set([lo,hi,0,1,-1,2,3])):
  if not lo<=value<=hi:continue
  for phase,drive in itertools.product([-1,0,2,3,2147483647],[0,1]):
   t=dict(s,phase=phase,drive=drive);t[key]=value;add(t)
for phase,drive in itertools.product([-1,0,2,3,2147483647],[0,1]):
 for offset in [-1,0,1]:add(dict(s,phase=phase,drive=drive,clock=D['clock'][1]+min(offset,0),reset_due=D['clock'][1]-max(offset,0)))
rng=random.Random(201172)
for n in range(1000):
 t={k:rng.choice([lo,hi,0,1]) for k,(lo,hi) in D.items()}
 t.update(clock=1000000000,reset_due=rng.choice([0,1000000001,1000000000,1000000000-rng.randrange(4096)*225792]),phase=rng.choice([-2147483648,-4000,-1,0,1,2,3,4,2147483647]))
 for key in ['stat','mode','paused','muted']:t[key]=rng.randrange(256)
 add(t)
with Path(sys.argv[1]).open('x') as f:json.dump(dict(schema='t172-cd-reset-experiment-v1',cases=cases),f)
print(len(cases))
