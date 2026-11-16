"""Independent head-state model derived from opaque boundary observations."""
import json
from pathlib import Path
def update(state):
 s=state.copy()
 if not s['drive'] or not s['valid']:return s
 period=225792 if s['mode']&128 else 451584
 n=0 if s['clock']<s['due'] else (s['clock']-s['due'])//period+1
 limit=s['target']+(1 if s['hold'] else -1)
 for _ in range(n):
  old=s['lba'];s['subq']=old;s['lba']=max(-150,old+1 if old<limit else old-8)
 s['due']+=n*period
 return s
def expected_rows(matrix):
 for case in json.loads(Path(matrix).read_text())['cases']:yield dict(case_id=case['id'],state=update(case['state']))
