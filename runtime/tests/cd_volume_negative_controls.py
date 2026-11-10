"""Reject altered samples, guards, coefficients, identities and invalid domains."""
import copy
import json
import subprocess
import sys
import tempfile
from pathlib import Path
from validate_cd_volume import compare
plan=json.loads(Path(sys.argv[1]).read_text());job=plan['jobs'][0]
original=[json.loads(l) for l in Path(job['candidate']['trace']).read_text().splitlines()];results=[]
scratch=Path(__file__).resolve().parents[2]/'_scratch';scratch.mkdir(exist_ok=True)
with tempfile.TemporaryDirectory(prefix='cd-volume-negative-',dir=scratch) as temp:
 work=Path(temp)
 def check(name,change):
  rows=copy.deepcopy(original);p=copy.deepcopy(plan);j=copy.deepcopy(job);change(rows,p,j)
  out=work/(name+'.jsonl');out.write_text(''.join(json.dumps(r)+'\n' for r in rows));j['candidate']['trace']=str(out)
  try:compare(p,j)
  except (ValueError,KeyError,IndexError):results.append(dict(name=name,rejected=True))
  else:raise AssertionError('accepted '+name)
 for field in ('matrix_sha256','binary_sha256','adapter_sha256'):
  check(field,lambda a,p,j,f=field:a[0]['metadata'].__setitem__(f,'0'*64))
 check('source-identity',lambda a,p,j:a[0]['metadata']['source'].__setitem__('commit','0'*40))
 check('expected-source',lambda a,p,j:p.__setitem__('tested_commit','0'*40))
 check('row-count',lambda a,p,j:a.pop())
 check('row-order',lambda a,p,j:a.__setitem__(slice(1,3),a[1:3][::-1]))
 check('schema',lambda a,p,j:a[0]['metadata'].__setitem__('schema','wrong'))
 for field in ('pending','active','samples','prefix','suffix'):
  check(field,lambda a,p,j,f=field:a[5][f].__setitem__(0,a[5][f][0]+1))
 check('unused-tail',lambda a,p,j:a[5]['samples'].__setitem__(-1,0))
 check('case-id',lambda a,p,j:a[5].__setitem__('case_id','wrong'))
 invalid=[dict(op='apply',frames=1,null=1),dict(op='apply',frames=2),dict(op='apply',frames=8193),dict(op='apply',frames=-2147483649),dict(op='apply',frames=0,null=2),dict(op='samples',values=[32768]),dict(op='samples',values=[-32769]),dict(op='samples',values=[0]*16385),dict(op='active',values=[0,0,0]),dict(op='active',values=[256,0,0,0]),dict(op='pending',values=[-1,0,0,0]),dict(op='not-an-operation')]
 for n,op in enumerate(invalid):
  matrix=work/f'invalid-{n}.json';matrix.write_text(json.dumps(dict(schema='t172-cd-volume-experiment-v1',cases=[dict(id='invalid',operations=[dict(op='samples',values=[1,2]),op])])))
  result=subprocess.run([sys.executable,plan['runner'],str(matrix),str(work/f'invalid-{n}.jsonl'),'--executable',job['baseline']['executable'],'--identity',job['baseline']['identity']],capture_output=True)
  if result.returncode==0:raise AssertionError('accepted invalid '+str(n))
  results.append(dict(name='invalid-'+str(n),rejected=True))
with Path(sys.argv[2]).open('x') as f:json.dump(results,f,indent=2)
print(json.dumps(dict(rejected=len(results))))
