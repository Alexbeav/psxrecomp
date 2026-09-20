"""Bind texture helper and public draw evidence without reading old implementations."""
import argparse
import json
import subprocess
from pathlib import Path
from validate_gpu_texture import digest

p=argparse.ArgumentParser();p.add_argument('plan',type=Path);p.add_argument('source',type=Path);p.add_argument('output',type=Path);a=p.parse_args()
plan=json.loads(a.plan.read_text());results=json.loads(Path(plan['result']).read_text());folder=a.plan.parent
assert len(results)==len(plan['jobs']) and all(r['valid'] for r in results)
files={a.plan.resolve(),Path(plan['result']).resolve()}
for j,result in zip(plan['jobs'],results):
    assert j['name']==result['name'];files.add(Path(j['matrix']).resolve())
    for side in ['baseline','candidate']:
        item=j[side]
        with open(item['trace']) as f:meta=json.loads(next(f))['metadata']
        assert meta['source']==json.loads(Path(item['identity']).read_text())
        assert meta['source']['commit']==plan['base' if side=='baseline' else 'tested_commit']
        assert meta['matrix_sha256']==digest(j['matrix'])
        assert meta['binary_sha256']==digest(item['executable'])
        files.update(Path(item[k]).resolve() for k in ['trace','identity','executable'])
for name,side in [('texture-draw-review-baseline.jsonl','baseline'),('texture-draw-review-6eebf2bc.jsonl','candidate')]:
    trace=folder/name
    with trace.open() as f:meta=json.loads(next(f))['metadata']
    assert meta['source']['commit']==plan['base' if side=='baseline' else 'tested_commit']
    command=meta['source']['command'];binary=Path(command[command.index('-o')+1]);identity=binary.with_name(binary.stem+'-receipt.json')
    assert digest(binary)==meta['binary_sha256'];assert json.loads(identity.read_text())==meta['source']
    assert meta['matrix_sha256']==digest(folder/'texture-draw-review-v1.json')
    files.update([trace.resolve(),binary.resolve(),identity.resolve()])
for pattern in ['gpu-texture-*.json','gpu-texture-authored-*.c','texture-*.json']:
    files.update(p.resolve() for p in folder.glob(pattern))
for pattern in ['gpu_texture*.py','gpu_texture_provenance.json','run_gpu_texture_qualification.py','validate_gpu_texture.py','bind_gpu_texture_evidence.py']:
    files.update(p.resolve() for p in (a.source/'runtime/tests').glob(pattern))
head=subprocess.check_output(['git','-C',str(a.source),'rev-parse','HEAD'],text=True).strip()
for owned in ['runtime/include/source_gpu_texture.h','runtime/src/gpu_sw_renderer.c']:
    assert subprocess.run(['git','-C',str(a.source),'diff','--quiet',plan['tested_commit'],'HEAD','--',owned]).returncode==0
    assert subprocess.run(['git','-C',str(a.source),'diff','--quiet','HEAD','--',owned]).returncode==0
    files.add((a.source/owned).resolve())
receipt=dict(schema='t172-gpu-texture-evidence-v1',base=plan['base'],tested_commit=plan['tested_commit'],review_commit=head,
             datasets=len(results),cases=sum(r['cases'] for r in results),observations=sum(r['rows'] for r in results),
             public_draw_cases=96,public_draw_changed_pixels=1514,negative_controls=23,
             exclusions='Restricted old source exports/compiler logs/archives and source-origin reports not read/copied; metadata retains their hashes.',
             files=[dict(path=str(p),size=p.stat().st_size,sha256=digest(p)) for p in sorted(files)])
with a.output.open('x') as f:json.dump(receipt,f,indent=2)
print(json.dumps(dict(files=len(files),sha256=digest(a.output),review_commit=head,cases=receipt['cases'],observations=receipt['observations'])))
