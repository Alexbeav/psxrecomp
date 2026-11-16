"""Corrupt individual evidence dimensions and reject invalid observer inputs."""
import copy
import json
import subprocess
import sys
import tempfile
from pathlib import Path
from validate_cd_delivery import compare
plan=json.loads(Path(sys.argv[1]).read_text());job=next(j for j in plan['jobs'] if j['name']=='helpers-O0')
original=[json.loads(l) for l in Path(job['candidate']['trace']).read_text().splitlines()];results=[]
scratch=Path(__file__).resolve().parents[2]/'_scratch';scratch.mkdir(exist_ok=True)
with tempfile.TemporaryDirectory(prefix='cd-delivery-negative-',dir=scratch) as temp:
 work=Path(temp)
 def check(name,change):
  rows=copy.deepcopy(original);p=copy.deepcopy(plan);j=copy.deepcopy(job);change(rows,p,j)
  out=work/(name+'.jsonl');out.write_text(''.join(json.dumps(r)+'\n' for r in rows));j['candidate']['trace']=str(out)
  try:compare(p,j)
  except (ValueError,KeyError,IndexError):results.append(dict(name=name,rejected=True))
  else:raise AssertionError('accepted '+name)
 for field in ('matrix_sha256','binary_sha256','adapter_sha256'):
  check(field,lambda a,p,j,f=field:a[0]['metadata'].__setitem__(f,'0'*64))
 check('expected-source',lambda a,p,j:p.__setitem__('tested_commit','0'*40))
 check('source-identity',lambda a,p,j:a[0]['metadata']['source'].__setitem__('commit','0'*40))
 check('schema',lambda a,p,j:a[0]['metadata'].__setitem__('schema','wrong'))
 check('row-count',lambda a,p,j:a.pop())
 check('row-order',lambda a,p,j:a.__setitem__(slice(1,3),a[1:3][::-1]))
 for field in ('external','counters','fill','response'):
  check('state-'+field,lambda a,p,j,f=field:a[15]['state'][f].__setitem__(0,a[15]['state'][f][0]+1))
 index=next(i for i,r in enumerate(original) if len(r.get('events',[]))>=6)
 for arg in range(3):
  check('callback-arg-'+str(arg),lambda a,p,j,n=arg:a[index]['events'][0]['args'].__setitem__(n,a[index]['events'][0]['args'][n]+1))
 check('callback-entry',lambda a,p,j:a[index]['events'][0]['state']['external'].__setitem__(0,7))
 check('callback-slot-hash',lambda a,p,j:a[index]['events'][0]['state']['slots'][0].__setitem__(2,'0'*64))
 check('callback-order',lambda a,p,j:a[index]['events'].reverse())
 check('missing-callback',lambda a,p,j:a[index]['events'].pop())
 check('return',lambda a,p,j:a[index].__setitem__('return_value',8))
 dump=next(i for i,r in enumerate(original) if r.get('payload'))
 check('payload-tail',lambda a,p,j:a[dump].__setitem__('payload',a[dump]['payload'][:-2]+('01' if a[dump]['payload'][-2:]!='01' else '02')))
 check('payload-hash',lambda a,p,j:a[dump]['state']['slots'][0].__setitem__(2,'0'*64))
 invalid=[dict(op='end_state',value=-2**31-1),dict(op='end_state',value=2**31),dict(op='media',delivered=2,have_raw=0,user_seed=0,raw_seed=0),dict(op='media',delivered=1,have_raw=2,user_seed=0,raw_seed=0),dict(op='media',delivered=1,have_raw=1,user_seed=2**32,raw_seed=0),dict(op='slot',slot=8,size=0,pos=0,seed=0),dict(op='slot',slot=0,size=2341,pos=0,seed=0),dict(op='slot',slot=0,size=1,pos=2,seed=0),dict(op='set',field='s_ring_read',value=8),dict(op='set',field='read_min',value=-1),dict(op='set',field='read_sec',value=65536),dict(op='set',field='s_dataready_fires',value=2**64),dict(op='immediate',sequence=-1),dict(op='dump',slot=8)]
 for n,op in enumerate(invalid):
  matrix=work/f'invalid-{n}.json';matrix.write_text(json.dumps(dict(schema='t172-cd-delivery-experiment-v1',cases=[dict(id='invalid',operations=[op])])))
  result=subprocess.run([sys.executable,plan['runner'],str(matrix),str(work/f'invalid-{n}.jsonl'),'--executable',job['baseline']['executable'],'--identity',job['baseline']['identity']],capture_output=True)
  if result.returncode==0:raise AssertionError('accepted invalid '+str(n))
  results.append(dict(name='invalid-'+str(n),rejected=True))
with Path(sys.argv[2]).open('x') as f:json.dump(results,f,indent=2)
print(json.dumps(dict(rejected=len(results))))
