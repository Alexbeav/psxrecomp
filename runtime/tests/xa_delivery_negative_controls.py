"""Reject altered call entry states, event order, PCM and fixture domains."""
import copy,json,subprocess,sys,tempfile
from pathlib import Path
from validate_xa_delivery import compare
plan=json.loads(Path(sys.argv[1]).read_text());job=copy.deepcopy(plan['jobs'][0]);results=[]
scratch=Path(__file__).resolve().parents[2]/'_scratch';scratch.mkdir(exist_ok=True)
with tempfile.TemporaryDirectory(prefix='xa-delivery-negative-',dir=scratch) as temp:
 work=Path(temp);matrix=work/'minimal.json'
 good=dict(id='control',operations=[dict(op='controls',values=[64,3,4,0]),dict(op='delivery',values=[2,3,4,100,1,0,0,0]),dict(op='call',lba=-172)])
 matrix.write_text(json.dumps(dict(schema='t172-xa-delivery-experiment-v1',cases=[good])));job['matrix']=str(matrix)
 for side in ('baseline','candidate'):job[side]['trace']=str(work/(side+'.jsonl'))
 compare(plan,job);original=[json.loads(l) for l in Path(job['candidate']['trace']).read_text().splitlines()]
 def check(name,change):
  rows=copy.deepcopy(original);p=copy.deepcopy(plan);j=copy.deepcopy(job);change(rows,p,j)
  out=work/(name+'.jsonl');out.write_text(''.join(json.dumps(r)+'\n' for r in rows));j['candidate']['trace']=str(out)
  try:compare(p,j)
  except (ValueError,KeyError,IndexError):results.append(dict(name=name,rejected=True))
  else:raise AssertionError('accepted '+name)
 for field in ('matrix_sha256','adapter_sha256'):
  check(field,lambda a,p,j,f=field:a[0]['metadata'].__setitem__(f,'0'*64))
 for field in ('commit','binary_sha256','dependencies_sha256'):
  check(field,lambda a,p,j,f=field:a[0]['metadata']['source'].__setitem__(f,'wrong'))
 check('expected-source',lambda a,p,j:p.__setitem__('tested_commit','0'*40))
 check('row-count',lambda a,p,j:a.pop())
 check('schema',lambda a,p,j:a[0]['metadata'].__setitem__('schema','wrong'))
 for field in ('delivery','input_prefix','input_suffix'):
  check(field,lambda a,p,j,f=field:a[-1][f].__setitem__(0,a[-1][f][0]+1))
 for field in ('controls','stream','history','active_volume','pending_volume'):
  check(field,lambda a,p,j,f=field:a[-1]['state'][f].__setitem__(0,a[-1]['state'][f][0]+1))
 check('return',lambda a,p,j:a[-1].__setitem__('return',0))
 check('input-hash',lambda a,p,j:a[-1].__setitem__('input_sha256','0'*64))
 check('event-order',lambda a,p,j:a[-1]['events'].__setitem__(slice(0,2),a[-1]['events'][0:2][::-1]))
 check('entry-state',lambda a,p,j:a[-1]['events'][1]['state']['history'].__setitem__(0,1))
 check('callback-argument',lambda a,p,j:a[-1]['events'][4]['args'].__setitem__(1,0))
 check('pcm-hash',lambda a,p,j:a[-1]['events'][3].__setitem__('pcm_sha256','0'*64))
 check('sink-sample',lambda a,p,j:a[-1]['events'][-2]['samples'].__setitem__(0,1))
 check('case-id',lambda a,p,j:a[-1].__setitem__('case_id','wrong'))
 invalid=[dict(op='history',values=[8388608,0,0,0]),dict(op='history',values=[-8388609,0,0,0]),dict(op='stream',values=[0,0,0,2147483648]),dict(op='controls',values=[256,0,0,0]),dict(op='delivery',values=[0]*7),dict(op='patch',writes=[[2352,0]]),dict(op='patch',writes=[[0,-1]]),dict(op='call',lba=2147483648),dict(op='call',lba=0,null_raw=2),dict(op='classify',have_raw=0,null_raw=-1),dict(op='seed',seed=-1),dict(op='unknown')]
 for n,op in enumerate(invalid):
  path=work/f'invalid-{n}.json';path.write_text(json.dumps(dict(schema='t172-xa-delivery-experiment-v1',cases=[dict(id='invalid',operations=[op])])))
  result=subprocess.run([sys.executable,plan['runner'],str(path),str(work/f'invalid-{n}.jsonl'),'--executable',job['baseline']['executable'],'--identity',job['baseline']['identity']],capture_output=True)
  if result.returncode==0:raise AssertionError('accepted invalid '+str(n))
  results.append(dict(name='invalid-'+str(n),rejected=True))
with Path(sys.argv[2]).open('x') as f:json.dump(results,f,indent=2)
print(json.dumps(dict(rejected=len(results))))
