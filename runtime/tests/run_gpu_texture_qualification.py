"""Run opaque renderer observers and validate synthetic texture behavior."""
import argparse
import json
import subprocess
from pathlib import Path
from validate_gpu_texture import read_trace,digest,require

p=argparse.ArgumentParser();p.add_argument('plan');a=p.parse_args();plan=json.loads(Path(a.plan).read_text());results=[]
for job in plan['jobs']:
    observed=[]
    for side in ['baseline','candidate']:
        item=job[side]
        if not Path(item['trace']).exists():
            subprocess.run(['python',plan['runner'],job['matrix'],item['trace'],'--executable',item['executable'],'--identity',item['identity']],capture_output=True,text=True,check=True)
        meta,rows=read_trace(job['matrix'],item['trace'],item['identity'])
        require(meta['source']['commit']==plan['base' if side=='baseline' else 'tested_commit'],'commit')
        require(meta['binary_sha256']==digest(item['executable'])==meta['source']['binary_sha256'],'binary')
        observed.append((meta,rows))
    require(observed[0][0]['source']['optimization']==observed[1][0]['source']['optimization'],'optimization')
    require(len(observed[0][1])==len(observed[1][1]),'length')
    for x,y in zip(observed[0][1],observed[1][1]):require(x==y,f"difference {x['case_id']} step{x['step']}")
    result=dict(name=job['name'],cases=observed[0][0]['cases'],rows=len(observed[0][1]),valid=True)
    results.append(result);print(json.dumps(result),flush=True)
Path(plan['result']).write_text(json.dumps(results,indent=2))
