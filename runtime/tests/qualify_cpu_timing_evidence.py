"""Verify the complete declared evidence set for one exact CPU implementation."""
import argparse
import hashlib
import json
from pathlib import Path
from validate_cpu_timing_trace import read_trace,check_pairs,require


def qualify(folder,plan_path):
    folder=Path(folder)
    entries=json.loads(Path(plan_path).read_text())
    results=[]
    for entry in entries:
        matrix=folder/entry['matrix']
        baseline=folder/entry['baseline'];candidate=folder/entry['candidate']
        ma,a=read_trace(matrix,baseline);mb,b=read_trace(matrix,candidate)
        require(ma['source']['commit']=='a697d2f4cf6e7f13b6fb61291b9737698ff68bdd','baseline identity')
        require(mb['source']['commit']=='ed855c9968ca2ae2ea87904d19c343e7de035cd7','candidate identity')
        require(ma['source']['profile']==mb['source']['profile']==entry['profile'],'profile')
        for x,y in zip(a,b):
            require(x==y,f"{entry['name']} differs at {x['case_id']} step {x['step']}")
        pairs=0
        if entry['kind'] in ['wire-restore','memory-restore']:
            prefix='wire-capture' if entry['kind']=='wire-restore' else 'memory'
            capture=folder/f"cpu-timing-{prefix}-{entry['profile']}-a697d2f4cf6e.jsonl"
            pairs=check_pairs(matrix,candidate,capture)
        result=dict(name=entry['name'],profile=entry['profile'],cases=ma['cases'],rows=len(a),
                    events=sum(len(r['events']) for r in a),
                    event_cpu_states=sum(len(r.get('event_cpu_states',[])) for r in a),
                    scope_states=sum(len(r.get('scope_states',[])) for r in a),
                    paired_restores=pairs,exact=True,
                    matrix=entry['matrix'],baseline=entry['baseline'],candidate=entry['candidate'],
                    matrix_sha256=hashlib.sha256(matrix.read_bytes()).hexdigest(),
                    baseline_sha256=hashlib.sha256(baseline.read_bytes()).hexdigest(),
                    candidate_sha256=hashlib.sha256(candidate.read_bytes()).hexdigest())
        results.append(result)
        print(entry['name'],'exact',len(a),flush=True)
    return dict(tested_commit='ed855c9968ca2ae2ea87904d19c343e7de035cd7',
                baseline_commit='a697d2f4cf6e7f13b6fb61291b9737698ff68bdd',
                datasets=results,totals={key:sum(r[key] for r in results) for key in
                ['cases','rows','events','event_cpu_states','scope_states','paired_restores']})


if __name__=='__main__':
    parser=argparse.ArgumentParser()
    parser.add_argument('folder');parser.add_argument('plan');parser.add_argument('output')
    args=parser.parse_args()
    output=Path(args.output)
    require(not output.exists(),'report already exists')
    report=qualify(args.folder,args.plan)
    output.write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report['totals']))
