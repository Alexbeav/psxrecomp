"""Independent TOC delay/state/event model using accepted seek semantics."""
import copy,json
from pathlib import Path
from cd_implicit_model import seek
def expected_rows(matrix):
 for c in json.loads(Path(matrix).read_text())['cases']:
  s=c['state'].copy();events=[]
  def event(kind,args=()):events.append(dict(kind=kind,args=list(args)+[0]*(5-len(args)),state=copy.deepcopy(s)))
  motor=int(bool(s['stat']&2));paused=int(bool(motor and not s['reading'] and not s['stat']&224))
  origin=s['last']
  if s['source_clock'] and not s['reading'] and s['paused']:
   event(1,[s['read_min'],s['read_sec'],s['read_sect']]);origin=(s['read_min']*60+s['read_sec'])*75+s['read_sect']-150;event(2,[origin])
  origin=max(0,origin)
  if s['drive'] and s['valid']:origin=s['lba']
  
  event(5,[origin,0,motor,paused,s['mode']]);result=seek(origin,0,motor,paused,s['mode'],s['drive']);event(6,[result])
  if s['source_clock']:event(7,[25000]);event(8,[s['jitter']]);result=min(2147483647,result+s['jitter'])
  yield dict(case_id=c['id'],result=result,state=s,events=events)
