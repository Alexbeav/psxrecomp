"""Independent reset model derived from authored opaque experiments."""
import json
from pathlib import Path
from cd_head_model import update as head_update

def run(state):
 s=state.copy();events=[]
 def event(kind,arg=0):events.append(dict(kind=kind,args=[arg,0,0,0,0],state=s.copy()))
 if s['source_clock'] and s['reset_due'] and s['clock']>=s['reset_due']:
  if s['drive']:
   steps=(s['clock']-s['reset_due'])//451584+1
   advance=min(steps,max(0,3-s['phase'])) if s['hold'] else 0
   if advance:
    s['lba']=s['phase']+advance-1;s['phase']+=advance;s['reset_due']+=advance*451584
   if steps>advance:
    s.update(paused=1,read_min=0,read_sec=2,read_sect=0,seek_min=0,seek_sec=2,seek_sect=0,phase=0,lba=(-6 if s['hold'] else -8),due=s['reset_due']+(451584 if s['hold'] else 903168),reset_due=0,stat=s['stat']&~64)
    event(1);s=head_update(s);event(2)
  else:
   s['reset_due']=0;event(3);event(4,s['ready'])
   if s['ready']:
    event(5);event(6,s['stat']);event(7,2);event(8)
   s['muted']=0;event(9);event(10)
   s.update(paused=1,mode=32,read_min=0,read_sec=2,read_sect=0,seek_min=0,seek_sec=2,seek_sect=0,setloc=0,far=0)
 return dict(result=0,state=s,events=events)

def expected_rows(matrix):
 for case in json.loads(Path(matrix).read_text())['cases']:yield dict(case_id=case['id'],**run(case['state']))
