"""Run opaque observers and compare authored observations without reading old code."""
import argparse
import concurrent.futures
import json
from pathlib import Path
import subprocess
import sys

from validate_cpu_timing_trace import read_trace


def run(job, evidence, tools, commit, label):
    tag = job['production']
    binary = tools / f"cpu-timing-candidate-{commit[:12]}-{job['profile']}-memory-{label}{'' if tag == 'off' else '-' + tag}"
    output = evidence / job['baseline'].replace('-baseline.jsonl', f'-{commit[:12]}.jsonl')
    if not output.exists():
        command = [sys.executable, str(tools/'run_cpu_timing_matrix.py'),
                   str(evidence/job['matrix']), str(output),
                   '--executable', str(binary)+'.exe', '--identity', str(binary)+'-receipt.json',
                   '--memory', '--observe-cpu', '--load-delay-default', job['load_delay'],
                   '--mmio-wait', job['mmio'], '--poll-proof', job['poll']]
        result = subprocess.run(command, capture_output=True, text=True)
        if result.returncode:
            return dict(baseline=job['baseline'], valid=False, observer_exit=result.returncode)
    _, reference = read_trace(evidence/job['matrix'], evidence/job['baseline'])
    _, candidate = read_trace(evidence/job['matrix'], output)
    mismatches=[]
    for a,b in zip(reference,candidate):
        if a != b:
            mismatches.append(dict(case=a['case_id'],step=a['step'],fields=[k for k in a if a[k]!=b[k]]))
            if len(mismatches)==5:break
    return dict(baseline=job['baseline'],candidate=output.name,valid=not mismatches,
                rows=len(candidate),mismatches=mismatches)


if __name__ == '__main__':
    p=argparse.ArgumentParser();p.add_argument('plan',type=Path);p.add_argument('tools',type=Path)
    p.add_argument('commit');p.add_argument('label');p.add_argument('report',type=Path)
    p.add_argument('--ordinary-only',action='store_true');p.add_argument('--filter',default='')
    args=p.parse_args();jobs=json.loads(args.plan.read_text())
    jobs=[j for j in jobs if (not args.ordinary_only or j['production']=='off') and args.filter in j['baseline']]
    with concurrent.futures.ThreadPoolExecutor(max_workers=3) as pool:
        results=list(pool.map(lambda j:run(j,args.plan.parent,args.tools,args.commit,args.label),jobs))
    with args.report.open('x') as f:json.dump(results,f,indent=2)
    print(json.dumps(dict(files=len(results),passed=sum(r['valid'] for r in results),
                          failures=[r for r in results if not r['valid']])))
    sys.exit(0 if all(r['valid'] for r in results) else 1)
