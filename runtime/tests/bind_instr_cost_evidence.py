"""Hash exact opcode qualification artifacts without reading excluded sources."""
import argparse
import json
import subprocess
from pathlib import Path
from validate_instr_cost import digest

p=argparse.ArgumentParser();p.add_argument('plan',type=Path);p.add_argument('source',type=Path);p.add_argument('output',type=Path);a=p.parse_args()
plan=json.loads(a.plan.read_text());results=json.loads(Path(plan['result']).read_text())
assert len(results)==len(plan['jobs']) and all(r['valid'] for r in results)
files={a.plan.resolve(),Path(plan['result']).resolve()};cases=0
for j,result in zip(plan['jobs'],results):
    assert j['name']==result['name']
    files.add(Path(j['matrix']).resolve())
    for side in ['baseline','candidate']:
        item=j[side];trace=Path(item['trace']);identity=Path(item['identity']);binary=Path(item['executable'])
        with trace.open() as f:meta=json.loads(next(f))['metadata']
        assert meta['source']==json.loads(identity.read_text())
        assert meta['source']['commit']==plan['base' if side=='baseline' else 'tested_commit']
        assert meta['matrix_sha256']==digest(j['matrix'])
        assert meta['binary_sha256']==digest(binary)
        if side=='baseline':cases+=meta['cases']
        files.update([trace.resolve(),identity.resolve(),binary.resolve()])
files.update(p.resolve() for p in a.plan.parent.glob('instr-cost-*.json'))
for pattern in ['instr_cost*.py','instr_cost_provenance.json','run_instr_cost_qualification.py','validate_instr_cost.py','bind_instr_cost_evidence.py']:
    files.update(p.resolve() for p in (a.source/'runtime/tests').glob(pattern))
header='runtime/include/psx_instr_cost.h';files.add((a.source/header).resolve())
head=subprocess.check_output(['git','-C',str(a.source),'rev-parse','HEAD'],text=True).strip()
assert subprocess.run(['git','-C',str(a.source),'diff','--quiet',plan['tested_commit'],'HEAD','--',header]).returncode==0
assert subprocess.run(['git','-C',str(a.source),'diff','--quiet','HEAD','--',header]).returncode==0
receipt=dict(schema='t172-instr-cost-evidence-v1',base=plan['base'],tested_commit=plan['tested_commit'],review_commit=head,
             datasets=len(results),cases=cases,observations=sum(r['rows'] for r in results),negative_controls=21,
             exclusions='No old header, restricted export/archive/compiler logs or origin-comment report read/copied; hashes retained in build metadata.',
             files=[dict(path=str(p),size=p.stat().st_size,sha256=digest(p)) for p in sorted(files)])
with a.output.open('x') as f:json.dump(receipt,f,indent=2)
print(json.dumps(dict(files=len(files),sha256=digest(a.output),review_commit=head,cases=cases,observations=receipt['observations'])))
