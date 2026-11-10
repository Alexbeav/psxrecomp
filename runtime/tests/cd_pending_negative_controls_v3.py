"""Reject altered evidence, identities, callback arguments and invalid observations."""
import copy
import json
import subprocess
import sys
import tempfile
from pathlib import Path
from validate_cd_pending_v3 import compare

plan=json.loads(Path(sys.argv[1]).read_text());job=next(j for j in plan['jobs'] if j['name']=='edges-O0')
original=[json.loads(l) for l in Path(job['candidate']['trace']).read_text().splitlines()]
results=[]
scratch=Path(__file__).resolve().parents[2]/'_scratch';scratch.mkdir(exist_ok=True)
with tempfile.TemporaryDirectory(prefix='cd-pending-negative-',dir=scratch) as temp:
    work=Path(temp)
    def check(name, mutate):
        altered=copy.deepcopy(original); p=copy.deepcopy(plan); j=copy.deepcopy(job)
        mutate(altered,p,j)
        trace=work/(name+'.jsonl');trace.write_text(''.join(json.dumps(x)+'\n' for x in altered))
        j['candidate']['trace']=str(trace)
        try:compare(p,j)
        except (ValueError,KeyError,IndexError) as e:results.append(dict(name=name,rejected=True,reason=str(e)))
        else:raise AssertionError('accepted negative control '+name)
    check('matrix-digest',lambda a,p,j:a[0]['metadata'].__setitem__('matrix_sha256','0'*64))
    check('binary-digest',lambda a,p,j:a[0]['metadata'].__setitem__('binary_sha256','0'*64))
    check('source-identity',lambda a,p,j:a[0]['metadata']['source'].__setitem__('commit','0'*40))
    check('expected-source',lambda a,p,j:p.__setitem__('tested_commit','0'*40))
    check('observer-version',lambda a,p,j:a[0]['metadata'].__setitem__('schema','t77-cd-pending-observations-v1'))
    check('row-count',lambda a,p,j:a.pop())
    check('row-order',lambda a,p,j:a.__setitem__(slice(1,3),list(reversed(a[1:3]))))
    check('case-identity',lambda a,p,j:a[1].__setitem__('case_id','wrong'))
    check('adapter-identity',lambda a,p,j:a[0]['metadata'].__setitem__('adapter_sha256','0'*64))
    for field in ('pending','counters','external','response'):
        check('state-'+field,lambda a,p,j,f=field:a[8]['state'][f].__setitem__(0,a[8]['state'][f][0]^1))
    index=next(i for i,x in enumerate(original) if any(e['kind']==2 for e in x.get('events',[])))
    event_index=next(i for i,e in enumerate(original[index]['events']) if e['kind']==2)
    for arg in range(4):
        check('trace-arg-'+str(arg),lambda a,p,j,n=arg:a[index]['events'][event_index]['args'].__setitem__(n,a[index]['events'][event_index]['args'][n]^1))
    check('callback-entry',lambda a,p,j:a[index]['events'][0]['state']['counters'].__setitem__(0,9999))
    check('callback-order',lambda a,p,j:a[index]['events'].reverse())
    check('missing-callback',lambda a,p,j:a[index]['events'].pop())
    check('short-trace-args',lambda a,p,j:a[index]['events'][event_index]['args'].pop())
    invalid_ops=[{'op':'set','field':'pending_dataready','value':1},{'op':'set','field':'irq_flag','value':8},{'op':'set','field':'s_ring_write','value':-1},{'op':'set','field':'stat_reg','value':256},{'op':'set','field':'psx_cycle_count','value':2**64},{'op':'arrive','delivered':2,'sequence':0},{'op':'arrive','delivered':1,'sequence':-1},{'op':'response','read':0,'count':17,'bytes':[0]*16},{'op':'response','read':16,'count':0,'bytes':[0]*16},{'op':'response','read':0,'count':0,'bytes':[0]*15},{'op':'not-an-operation'}]
    for field,value in [('pending',2),('stat',256),('slot',8),('due',2**64),('sequence',-1)]:
        op=dict(op='notification_state',pending=0,stat=0,slot=0,due=0,sequence=0);op[field]=value;invalid_ops.append(op)
    for n,op in enumerate(invalid_ops):
        matrix=work/f'invalid-{n}.json';matrix.write_text(json.dumps(dict(schema='t172-cd-pending-experiment-v1',cases=[dict(id='invalid',operations=[op])])))
        result=subprocess.run([sys.executable,plan['runner'],str(matrix),str(work/f'invalid-{n}.jsonl'),'--executable',job['baseline']['executable'],'--identity',job['baseline']['identity']],capture_output=True)
        if result.returncode==0:raise AssertionError('accepted invalid input '+str(n))
        results.append(dict(name='invalid-input-'+str(n),rejected=True))
with Path(sys.argv[2]).open('x') as f:json.dump(results,f,indent=2)
print(json.dumps(dict(rejected=len(results))))
