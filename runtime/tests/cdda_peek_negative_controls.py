"""Reject corrupted reader arguments, stored state and invalid fixtures."""
import copy,json,subprocess,sys,tempfile
from pathlib import Path
from validate_cdda_peek import compare
plan=json.loads(Path(sys.argv[1]).read_text());job=copy.deepcopy(plan['jobs'][0]);results=[]
scratch=Path(__file__).resolve().parents[2]/'_scratch';scratch.mkdir(exist_ok=True)
with tempfile.TemporaryDirectory(prefix='cdda-peek-negative-',dir=scratch) as temp:
 work=Path(temp);matrix=work/'minimal.json'
 good=dict(id='control',operations=[dict(op='reader',**{'return':1},valid=1,values=[1]+list(range(11))),dict(op='peek',lba=-172)])
 matrix.write_text(json.dumps(dict(schema='t172-cdda-peek-experiment-v1',cases=[good])));job['matrix']=str(matrix)
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
 for field in ('commit','binary_sha256'):
  check(field,lambda a,p,j,f=field:a[0]['metadata']['source'].__setitem__(f,'wrong'))
 check('expected-source',lambda a,p,j:p.__setitem__('tested_commit','0'*40))
 check('row-count',lambda a,p,j:a.pop())
 check('schema',lambda a,p,j:a[0]['metadata'].__setitem__('schema','wrong'))
 for field in ('subq','prefix','suffix','unowned'):
  check(field,lambda a,p,j,f=field:a[-1]['state'][f].__setitem__(0,a[-1]['state'][f][0]+1))
 for field in ('available','position_valid','handle'):
  check(field,lambda a,p,j,f=field:a[-1]['state'].__setitem__(f,a[-1]['state'][f]+1))
 for i in range(6):check('reader-arg-'+str(i),lambda a,p,j,i=i:a[-1]['events'][0]['args'].__setitem__(i,a[-1]['events'][0]['args'][i]+1))
 check('entry-state',lambda a,p,j:a[-1]['events'][0]['state'].__setitem__('available',1))
 check('return',lambda a,p,j:a[-1].__setitem__('return',0))
 check('extra-event',lambda a,p,j:a[-1]['events'].append(copy.deepcopy(a[-1]['events'][0])))
 invalid=[dict(op='peek',lba=2147483648),dict(op='peek',lba=-2147483649),dict(op='handle',value=3),dict(op='stored',available=0,position_valid=0,values=[0]*11),dict(op='reader',**{'return':1},valid=1,write_valid=1,write_bytes=0,values=[1]*12),dict(op='reader',**{'return':2147483648},valid=0,values=[0]*12),dict(op='reader',**{'return':0},valid=0,write_valid=2,values=[0]*12),dict(op='reader',**{'return':0},valid=0,values=[256]*12),dict(op='unknown')]
 for n,op in enumerate(invalid):
  path=work/f'invalid-{n}.json';path.write_text(json.dumps(dict(schema='t172-cdda-peek-experiment-v1',cases=[dict(id='invalid',operations=[op])])))
  result=subprocess.run([sys.executable,plan['runner'],str(path),str(work/f'invalid-{n}.jsonl'),'--executable',job['baseline']['executable'],'--identity',job['baseline']['identity']],capture_output=True)
  if result.returncode==0:raise AssertionError('accepted invalid '+str(n))
  results.append(dict(name='invalid-'+str(n),rejected=True))
with Path(sys.argv[2]).open('x') as f:json.dump(results,f,indent=2)
print(json.dumps(dict(rejected=len(results))))
