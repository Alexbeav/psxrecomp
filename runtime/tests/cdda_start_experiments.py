"""Independent playback-start field, selection and fatal-boundary fixtures."""
import json,itertools,sys
from pathlib import Path
suite=sys.argv[2];cases=[]
def add(fields=None,fix=None,req=0):
 f=dict(track_count=3,start_lba=300,selected_track=2,audio=1,delay=100,jitter=7,peek_at=0,values=list(range(160,172)));f.update(fix or {})
 cases.append(dict(id=f'{suite}-{len(cases)}',operations=[*[dict(op='set',field=k,value=v) for k,v in (fields or {}).items()],dict(op='fixture',**f),dict(op='start',requested_track=req)]))
if suite=='fields':
 domains=json.loads(Path(sys.argv[3]).read_text())
 for field,(lo,hi) in domains.items():
  for value in sorted(set([lo,hi,0,1]+([2,255] if hi>=255 else []))):add({field:value},req=1)
 for stat,paused in itertools.product([0,1,2,4,8,16,32,64,128,255],[0,1,255]):add(dict(stat_reg=stat,s_source_seek_paused=paused,cdda_lba=444,read_min=3,read_sec=4,read_sect=5),req=1)
elif suite=='selection':
 for req,playing,pending in itertools.product([-2147483648,-1,0,1,3,4,9,10,2147483647],[-1,0,1],[0,1]):add(dict(cdda_playing=playing,setloc_pending=pending,s_setloc_lba=750,read_min=1,read_sec=2,read_sect=3,cdda_lba=999),req=req)
 for lba in [-2147483648,-151,-150,-1,0,1,449999]:add(dict(setloc_pending=1,s_setloc_lba=lba),req=0)
 for track in [-2147483648,-1,0,1,3,4,9,10,2147483647]:add(fix=dict(selected_track=track),req=1)
 for count in [-2147483648,-1,0,1,9,10,2147483647]:add(fix=dict(track_count=count),req=1)
elif suite=='timing':
 for peek in range(-1,32):add(fix=dict(peek_at=peek),req=1)
 for delay,jitter in itertools.product([0,1,2147458647,2147483646,2147483647],[0,1,24999]):add(fix=dict(delay=delay,jitter=jitter),req=1)
 for audio in [-2147483648,-1,0,1,2147483647]:add(fix=dict(audio=audio),req=1)
with Path(sys.argv[1]).open('x') as f:json.dump(dict(schema='t172-cdda-start-experiment-v1',cases=cases),f)
print(suite,len(cases),sum(len(c['operations'])+1 for c in cases))
