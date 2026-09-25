"""Reject corrupted observations and out-of-contract fixture inputs."""
import copy,json,subprocess,sys,tempfile
from pathlib import Path
from validate_xa4 import compare
plan=json.loads(Path(sys.argv[1]).read_text());job=copy.deepcopy(plan['jobs'][1]);results=[]
scratch=Path(__file__).resolve().parents[2]/'_scratch';scratch.mkdir(exist_ok=True)
with tempfile.TemporaryDirectory(prefix='xa4-negative-',dir=scratch) as temp:
 work=Path(temp);matrix=work/'minimal.json'
 matrix.write_text(json.dumps(dict(schema='t172-xa4-experiment-v1',cases=[dict(id='negative-probe',operations=[dict(op='input_seed',seed=713),dict(op='output_seed',seed=128),dict(op='history',values=[8388607,-8388608,31,-17]),dict(op='stereo'),dict(op='mono')])])));job['matrix']=str(matrix)
 for side in ('baseline','candidate'):job[side]['trace']=str(work/(side+'.jsonl'))
 compare(plan,job)
 original=[json.loads(l) for l in Path(job['candidate']['trace']).read_text().splitlines()]
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
 for field in ('history','samples','input_prefix','input_suffix','output_prefix','output_suffix'):
  check(field,lambda a,p,j,f=field:a[5][f].__setitem__(0,a[5][f][0]+1))
 check('unused-stereo-tail',lambda a,p,j:a[5]['samples'].__setitem__(-1,0))
 check('frames',lambda a,p,j:a[5].__setitem__('frames',4032))
 check('input-preservation',lambda a,p,j:a[5].__setitem__('input_sha256','0'*64))
 check('case-id',lambda a,p,j:a[5].__setitem__('case_id','wrong'))
 invalid=[dict(op='history',values=[8388608,0,0,0]),dict(op='history',values=[-8388609,0,0,0]),dict(op='history',values=[0,0,0]),dict(op='patch',writes=[[2304,0]]),dict(op='patch',writes=[[-1,0]]),dict(op='patch',writes=[[0,256]]),dict(op='patch',writes=[[0,-1]]),dict(op='input_seed',seed=-1),dict(op='output_seed',seed=4294967296),dict(op='unknown')]
 for n,op in enumerate(invalid):
  path=work/f'invalid-{n}.json';path.write_text(json.dumps(dict(schema='t172-xa4-experiment-v1',cases=[dict(id='invalid',operations=[op])])))
  result=subprocess.run([sys.executable,plan['runner'],str(path),str(work/f'invalid-{n}.jsonl'),'--executable',job['baseline']['executable'],'--identity',job['baseline']['identity']],capture_output=True)
  if result.returncode==0:raise AssertionError('accepted invalid '+str(n))
  results.append(dict(name='invalid-'+str(n),rejected=True))
with Path(sys.argv[2]).open('x') as f:json.dump(results,f,indent=2)
print(json.dumps(dict(rejected=len(results))))
