"""Independent return/state/event model with accepted head and seek semantics."""
import copy,json
from pathlib import Path
from cd_head_model import update as head_update
HEAD='drive valid lba target hold subq mode clock due'.split()
def seek(origin,target,motor,paused,mode,profile):
 distance=abs(target-(origin if motor else 0));cycles=max(20000,distance*1568//15)
 if not motor:cycles+=33868800
 if distance>=2250:cycles+=10160640
 elif paused:cycles+=2475904//(2 if mode&128 else 1)
 elif profile and 3<=distance<=11:cycles+=1806336//(2 if mode&128 else 1)
 return min(2147483647,cycles)
def expected_rows(matrix):
 for c in json.loads(Path(matrix).read_text())['cases']:
  s=c['state'].copy();events=[]
  def event(kind,args=()):events.append(dict(kind=kind,args=list(args)+[0]*(5-len(args)),state=copy.deepcopy(s)))
  if s['source_clock']:
   event(1,[s['read_min'],s['read_sec'],s['read_sect']]);pos=(s['read_min']*60+s['read_sec'])*75+s['read_sect']-150;event(2,[pos]);origin=max(0,pos);target=max(0,s['setloc'] if s['pending'] else origin)
   event(3);s.update(head_update({k:s[k] for k in HEAD}));event(4)
   if s['drive'] and s['valid']:origin=s['lba']
   event(5,[origin,target,int(bool(s['stat']&2)),s['paused'],s['mode']]);delay=seek(origin,target,bool(s['stat']&2),s['paused'],s['mode'],s['drive']);event(6,[delay]);s['valid']=0;event(7,[25000]);event(8,[s['jitter']]);result=min(2147483647,delay+s['jitter'])
  elif s['pending']:
   origin=max(0,s['last']);target=s['setloc'];event(5,[origin,target,int(bool(s['stat']&2)),s['paused'],s['mode']]);delay=seek(origin,target,bool(s['stat']&2),s['paused'],s['mode'],s['drive']);event(6,[delay]);event(9,[delay]);event(10,[s['speed_result']]);result=s['speed_result']
  else:result=0
  yield dict(case_id=c['id'],result=result,state=s,events=events)
