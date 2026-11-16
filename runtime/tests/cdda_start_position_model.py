"""Model the separate six-step defensive position fixture."""
import json
from pathlib import Path
from cdda_start_model_v3 import expected_rows as normal_rows

def expected_rows(matrix):
 data=json.loads(Path(matrix).read_text());cases=[]
 for c in data['cases']:
  track=c['source']=='track'
  cases.append(dict(id=c['id'],operations=[dict(op='set',field='cdda_playing',value=1),dict(op='set',field='source_seeking',value=1),dict(op='set',field='cdda_lba',value=0 if track else c['position']),dict(op='fixture',track_count=1,start_lba=c['position'] if track else 0,selected_track=1,audio=1,delay=100,jitter=0,peek_at=0,values=list(range(12))),dict(op='start',requested_track=int(track))]))
 for row in normal_rows(dict(cases=cases)):
  row['step']+=1
  yield row
