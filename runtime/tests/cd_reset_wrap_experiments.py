"""Exact admitted wrap-to-zero sentinel cases, not general wrap qualification."""
import itertools,json,sys
from pathlib import Path
E=Path(sys.argv[1]);s=json.loads((E/'cd-reset-probes-v1.json').read_text())['cases'][0]['state'];cases=[]
for phase,source,drive,hold,mode in itertools.product(range(3),[1,-1,-2147483648],[1,-1],[1,-1],[0,128,255]):
 cases.append(dict(id=str(len(cases)),state=dict(s,phase=phase,source_clock=source,drive=drive,hold=hold,valid=0,mode=mode,clock=2**64-451584,reset_due=2**64-451584)))
with (E/'cd-reset-wrap-v1.json').open('x') as f:json.dump(dict(schema='t172-cd-reset-experiment-v1',cases=cases),f)
print(len(cases))
