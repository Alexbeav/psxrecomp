"""Reject changed states, callback order, payload and invalid fixture domains."""
import copy,json,subprocess,sys,tempfile
from pathlib import Path
from validate_cdda_notification import compare
plan=json.loads(Path(sys.argv[1]).read_text());job=copy.deepcopy(plan['jobs'][0]);results=[]
scratch=Path(__file__).resolve().parents[2]/'_scratch';scratch.mkdir(exist_ok=True)
with tempfile.TemporaryDirectory(prefix='cdda-notification-negative-',dir=scratch) as temp:
 work=Path(temp);matrix=work/'minimal.json'
 good=dict(id='control',operations=[dict(op='notification',enabled=1,type=4,count=1,values=list(range(11,19))),dict(op='present')])
 matrix.write_text(json.dumps(dict(schema='t172-cdda-notification-experiment-v1',cases=[good])));job['matrix']=str(matrix)
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
 for field in ('notification','external','response','input','unowned'):
  check(field,lambda a,p,j,f=field:a[-1]['state'][f].__setitem__(0,a[-1]['state'][f][0]+1))
 check('payload-tail',lambda a,p,j:a[-1]['state']['notification'].__setitem__(-1,0))
 check('response-tail',lambda a,p,j:a[-1]['state']['response'].__setitem__(-1,1))
 check('pipe-hash',lambda a,p,j:a[-1]['state']['unowned'].__setitem__(-1,'0'*64))
 check('event-order',lambda a,p,j:a[-1]['events'].__setitem__(slice(0,2),a[-1]['events'][0:2][::-1]))
 check('entry-state',lambda a,p,j:a[-1]['events'][3]['state']['notification'].__setitem__(1,4))
 check('callback-argument',lambda a,p,j:a[-1]['events'][2].__setitem__('value',0))
 check('case-id',lambda a,p,j:a[-1].__setitem__('case_id','wrong'))
 invalid=[dict(op='queue',type=256,count=1),dict(op='queue',type=-1,count=1),dict(op='queue',type=1,count=9),dict(op='queue',type=1,count=-1),dict(op='input',values=[0]*7),dict(op='input',values=[256]*8),dict(op='notification',enabled=2147483648,type=1,count=1,values=[0]*8),dict(op='response',read=16,count=0,values=[0]*16),dict(op='response',read=0,count=17,values=[0]*16),dict(op='external',irq=0,source_clock=0,clock=-1,ready_due=0),dict(op='external',irq=0,source_clock=0,clock=0,ready_due=2**64),dict(op='unknown')]
 for n,op in enumerate(invalid):
  path=work/f'invalid-{n}.json';path.write_text(json.dumps(dict(schema='t172-cdda-notification-experiment-v1',cases=[dict(id='invalid',operations=[op])])))
  result=subprocess.run([sys.executable,plan['runner'],str(path),str(work/f'invalid-{n}.jsonl'),'--executable',job['baseline']['executable'],'--identity',job['baseline']['identity']],capture_output=True)
  if result.returncode==0:raise AssertionError('accepted invalid '+str(n))
  results.append(dict(name='invalid-'+str(n),rejected=True))
with Path(sys.argv[2]).open('x') as f:json.dump(results,f,indent=2)
print(json.dumps(dict(rejected=len(results))))
