"""Run only public-interface opcode observers, then validate exact raw results."""
import argparse
import json
import subprocess
from pathlib import Path
from validate_instr_cost import compare,digest

p=argparse.ArgumentParser();p.add_argument('plan');a=p.parse_args()
plan=json.loads(Path(a.plan).read_text());results=[]
for job in plan['jobs']:
    metadata=[]
    for side in ['baseline','candidate']:
        item=job[side]
        if not Path(item['trace']).exists():
            subprocess.run(['python',plan['runner'],job['matrix'],item['trace'],'--executable',item['executable'],'--identity',item['identity']],check=True,capture_output=True,text=True)
        with open(item['trace']) as f:meta=json.loads(next(f))['metadata']
        assert meta['binary_sha256']==digest(item['executable'])==meta['source']['binary_sha256']
        assert meta['source']['commit']==plan['base' if side=='baseline' else 'tested_commit']
        metadata.append(meta)
    assert all(metadata[0]['source'][k]==metadata[1]['source'][k] for k in ['language','optimization'])
    count=compare(job['matrix'],job['baseline']['trace'],job['candidate']['trace'],job['baseline']['identity'],job['candidate']['identity'])
    result=dict(name=job['name'],rows=count,valid=True);results.append(result);print(json.dumps(result),flush=True)
Path(plan['result']).write_text(json.dumps(results,indent=2))
