"""Bind qualified raw observations, opaque binaries and metadata without reading old sources."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess


def digest(path):
    with path.open('rb') as f:
        return hashlib.file_digest(f,'sha256').hexdigest()


def bind(plan_path, report_path, tools, source):
    folder=plan_path.parent
    plan=json.loads(plan_path.read_text());report=json.loads(report_path.read_text())
    files={plan_path.resolve(),report_path.resolve()}
    qualified={r['baseline']:r for r in report['datasets']}
    for entry in plan:
        result=qualified[entry['baseline']]
        assert result['valid'] and result['candidate']==entry['candidate']
        matrix=folder/entry['matrix'];files.add(matrix.resolve())
        for role in ['baseline','candidate']:
            trace=folder/entry[role];files.add(trace.resolve())
            with trace.open() as f:meta=json.loads(next(f))['metadata']
            expected=report['base'] if role=='baseline' else report['tested_commit']
            assert meta['source']['commit']==expected
            assert meta['matrix_sha256']==digest(matrix)
            assert meta['source']['profile']==entry['profile']
            assert meta['source'].get('memory_production','off')==entry['production']
            args=meta['source']['compile_args'];binary=Path(args[args.index('-o')+1])
            identity=binary.with_name(binary.stem+'-receipt.json')
            assert digest(binary)==meta['binary_sha256']==meta['source']['binary_sha256']
            assert json.loads(identity.read_text())==meta['source']
            files.update([binary.resolve(),identity.resolve()])
        if 'capture' in entry:files.add((folder/entry['capture']).resolve())
    for pattern in ['memory-timing-*.json','memory-timing-authored-fragment-*.c',
                    'memory-full-tu-check-*.json','install_memory_timing_fragment-*.py']:
        files.update(f.resolve() for f in folder.glob(pattern) if not f.name.startswith('validation-receipt'))
    for pattern in ['memory_timing*.py','compare_memory_timing.py','bind_memory_timing_evidence.py',
                    'memory_timing_provenance.json','validate_cpu_timing_trace.py','cpu_timing_validator_controls.py']:
        files.update(f.resolve() for f in (source/'runtime/tests').glob(pattern))
    files.add((source/'runtime/src/memory.c').resolve())
    tracked_head=subprocess.check_output(['git','-C',str(source),'rev-parse','HEAD'],text=True).strip()
    assert subprocess.run(['git','-C',str(source),'diff','--quiet',report['tested_commit'],'HEAD','--','runtime/src/memory.c']).returncode==0
    return dict(schema='t172-memory-timing-evidence-v1',base=report['base'],tested_commit=report['tested_commit'],
                review_commit=tracked_head,datasets=len(plan),totals=report['totals'],
                exclusions='Restricted exported old sources, archives and compiler logs are not read or copied; their metadata hashes remain in opaque build receipts.',
                files=[dict(path=str(p),size=p.stat().st_size,sha256=digest(p)) for p in sorted(files)])


if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('plan',type=Path);p.add_argument('report',type=Path)
    p.add_argument('tools',type=Path);p.add_argument('source',type=Path);p.add_argument('output',type=Path)
    a=p.parse_args();result=bind(a.plan,a.report,a.tools,a.source)
    with a.output.open('x') as f:json.dump(result,f,indent=2)
    print(json.dumps(dict(files=len(result['files']),sha256=digest(a.output),review_commit=result['review_commit'])))
