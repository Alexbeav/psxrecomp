"""Independent explicit-seek return/state/event model."""
import copy,json
from pathlib import Path
from cd_implicit_model import seek
def expected_rows(matrix):
 for c in json.loads(Path(matrix).read_text())['cases']:
  s=c['state'].copy();events=[]
  def event(kind,args=()):events.append(dict(kind=kind,args=list(args)+[0]*(5-len(args)),state=copy.deepcopy(s)))
  def msf(prefix):
   v=[s[prefix+k] for k in ['min','sec','sect']];event(1,v);n=(v[0]*60+v[1])*75+v[2]-150;event(2,[n]);return max(0,n)
  origin=msf('read_');target=msf('seek_')
  if s['reading']:origin+=2
  elif s['drive'] and s['valid']:origin=s['lba']
  s['valid']=0;motor=int(bool(s['stat']&2));event(5,[origin,target,motor,s['paused'],s['mode']]);result=seek(origin,target,motor,s['paused'],s['mode'],s['drive']);event(6,[result])
  if s['command']==21:result+=(225792 if s['mode']&128 else 451584)*(2 if s['drive'] else 1)
  if s['source_clock']:event(7,[25000]);event(8,[s['jitter']]);result+=s['jitter']
  yield dict(case_id=c['id'],result=min(2147483647,result),state=s,events=events)
