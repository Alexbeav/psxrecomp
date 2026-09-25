"""Bind cache qualification inputs without opening restricted reference source."""
import argparse
import hashlib
import json
import subprocess
from pathlib import Path

def digest(path):
    with path.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()

p=argparse.ArgumentParser();p.add_argument('plan',type=Path);p.add_argument('report',type=Path)
p.add_argument('source',type=Path);p.add_argument('output',type=Path);a=p.parse_args()
plan=json.loads(a.plan.read_text());report=json.loads(a.report.read_text());files={a.plan.resolve(),a.report.resolve(),Path(plan['result']).resolve()}
results=json.loads(Path(plan['result']).read_text())
assert len(results)==len(plan['jobs'])==report['datasets']
for job,result in zip(plan['jobs'],results):
    assert job['name']==result['name'] and result['differences']==0
    matrix=Path(job['matrix']);files.add(matrix.resolve())
    for side in ['baseline','candidate']:
        trace=Path(job[side]['trace']);binary=Path(job[side]['executable']);identity=Path(job[side]['identity'])
        with trace.open() as f:meta=json.loads(next(f))['metadata']
        assert meta['source']==json.loads(identity.read_text())
        assert meta['source']['commit']==report['base' if side=='baseline' else 'tested_commit']
        assert meta['matrix_sha256']==digest(matrix)
        assert meta['binary_sha256']==meta['source']['binary_sha256']==digest(binary)
        assert meta['icache_env']==job['environment']
        files.update([trace.resolve(),binary.resolve(),identity.resolve()])
    if 'capture' in job:files.add(Path(job['capture']).resolve())
folder=a.plan.parent
files.update(p.resolve() for p in folder.glob('icache-*.json'))
for pattern in ['icache*.py','icache_provenance.json','run_icache_qualification.py','bind_icache_evidence.py','validate_cpu_timing_trace.py']:
    files.update(p.resolve() for p in (a.source/'runtime/tests').glob(pattern))
owned=['runtime/src/psx_icache.c','runtime/include/psx_icache.h']
head=subprocess.check_output(['git','-C',str(a.source),'rev-parse','HEAD'],text=True).strip()
for path in owned:
    assert subprocess.run(['git','-C',str(a.source),'diff','--quiet',report['tested_commit'],'HEAD','--',path]).returncode==0
    assert subprocess.run(['git','-C',str(a.source),'diff','--quiet','HEAD','--',path]).returncode==0
    files.add((a.source/path).resolve())
receipt=dict(schema='t172-icache-evidence-v1',review_commit=head,**report,
             exclusions='No restricted source archives, extracted old implementations or compiler logs read/copied; their hashes remain in opaque receipts.',
             files=[dict(path=str(p),size=p.stat().st_size,sha256=digest(p)) for p in sorted(files)])
with a.output.open('x') as f:json.dump(receipt,f,indent=2)
print(json.dumps(dict(files=len(files),sha256=digest(a.output),review_commit=head)))
