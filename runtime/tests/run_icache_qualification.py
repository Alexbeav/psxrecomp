"""Execute opaque cache observers and compare authored experiment traces."""
import argparse
import json
import subprocess
from pathlib import Path

p=argparse.ArgumentParser()
p.add_argument('plan')
p.add_argument('--validate',action='store_true')
args=p.parse_args()
plan=json.loads(Path(args.plan).read_text())
if args.validate:
    from validate_cpu_timing_trace import read_trace
results=[]
for job in plan['jobs']:
    for side in ['baseline','candidate']:
        trace=Path(job[side]['trace'])
        if not trace.exists():
            command=['python',plan['runner'],job['matrix'],str(trace),'--icache','--observe-cpu',
                     '--icache-env',job['environment'],'--executable',job[side]['executable'],
                     '--identity',job[side]['identity']]
            run=subprocess.run(command,capture_output=True,text=True)
            if run.returncode:
                raise RuntimeError(run.stderr)
    if args.validate:
        bm,a=read_trace(job['matrix'],job['baseline']['trace'])
        cm,b=read_trace(job['matrix'],job['candidate']['trace'])
    else:
        def read(path):
            with open(path) as source:
                return json.loads(next(source))['metadata'],[json.loads(line) for line in source]
        bm,a=read(job['baseline']['trace']);cm,b=read(job['candidate']['trace'])
    assert bm['source']['profile']==cm['source']['profile']
    assert len(a)==len(b)
    differences=[]
    for x,y in zip(a,b):
        if x!=y:
            differences.append(dict(case=x['case_id'],step=x['step'],keys=[k for k in x if x[k]!=y[k]]))
    result=dict(name=job['name'],rows=len(a),differences=len(differences),first=differences[:3])
    results.append(result)
    print(json.dumps(result),flush=True)
Path(plan['result']).write_text(json.dumps(results,indent=2))
if any(r['differences'] for r in results):raise SystemExit(1)
