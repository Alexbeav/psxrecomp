"""Reject altered implicit-seek return, state, event order and fixture domains."""
import copy,json,subprocess,sys,tempfile
from pathlib import Path
from validate_cd_implicit import compare
p=json.loads(Path(sys.argv[1]).read_text());job=copy.deepcopy(p['jobs'][0]);results=[]
scratch=Path(__file__).resolve().parents[2]/'_scratch';scratch.mkdir(exist_ok=True)
with tempfile.TemporaryDirectory(dir=scratch,prefix='implicit-negative-') as temp:
 root=Path(temp);original=[json.loads(l) for l in Path(job['candidate']['trace']).read_text().splitlines()]
 def check(name,change):
  rows=copy.deepcopy(original);plan=copy.deepcopy(p);j=copy.deepcopy(job);change(rows,plan,j);out=root/(name+'.jsonl');out.write_text(''.join(json.dumps(r)+'\n' for r in rows));j['candidate']['trace']=str(out)
  try:compare(plan,j)
  except (ValueError,KeyError,IndexError):results.append(dict(name=name,rejected=True))
  else:raise AssertionError('accepted '+name)
 def event(a,kind):return next(e for r in a[1:] for e in r['events'] if e['kind']==kind)
 for key in ('schema','matrix_sha256','adapter_sha256'):check(key,lambda a,p,j,k=key:a[0]['metadata'].__setitem__(k,'wrong'))
 for key in ('commit','binary_sha256','dependencies_sha256','seek_header_sha256'):check(key,lambda a,p,j,k=key:a[0]['metadata']['source'].__setitem__(k,'wrong'))
 check('expected-commit',lambda a,p,j:p.__setitem__('tested_commit','wrong'));check('rows',lambda a,p,j:a.pop());check('case-id',lambda a,p,j:a[-1].__setitem__('case_id','wrong'));check('result',lambda a,p,j:a[-1].__setitem__('result',0))
 for key in original[-1]['state']:check(key,lambda a,p,j,k=key:a[-1]['state'].__setitem__(k,a[-1]['state'][k]+1))
 for kind in (1,2,5,6,7,8,9,10):check('args-'+str(kind),lambda a,p,j,k=kind:event(a,k)['args'].__setitem__(0,event(a,k)['args'][0]+1))
 check('head-entry',lambda a,p,j:event(a,3)['state'].__setitem__('valid',99))
 check('head-exit',lambda a,p,j:event(a,4)['state'].__setitem__('lba',99))
 check('clear-before-jitter',lambda a,p,j:event(a,7)['state'].__setitem__('valid',1))
 check('order',lambda a,p,j:a[-1]['events'].reverse())
 valid=json.loads(Path(job['matrix']).read_text())['cases'][-1]['state']
 changes=[dict(read_min=-65536),dict(read_sec=65536),dict(read_sect=65536),dict(jitter=25000),dict(lba=-151),dict(lba=450000),dict(mode=256),dict(clock=-1),dict(due=2**64-451584),dict(source_clock=2**31),dict(speed_result=2**31),dict(hold=1,target=2147483646),dict(source_clock=1,drive=1,valid=1,due=0,clock=451584*4096,mode=0)]
 for n,delta in enumerate(changes):
  inp=root/f'invalid-{n}.json';inp.write_text(json.dumps(dict(schema='t172-cd-implicit-experiment-v1',cases=[dict(id='invalid',state=dict(valid,**delta))])))
  r=subprocess.run([sys.executable,p['runner'],str(inp),str(root/f'invalid-{n}.jsonl'),'--executable',job['baseline']['executable'],'--identity',job['baseline']['identity']],capture_output=True);assert r.returncode!=0;results.append(dict(name=f'invalid-{n}',rejected=True))
with Path(sys.argv[2]).open('x') as f:json.dump(results,f,indent=2)
print(json.dumps(dict(rejected=len(results))))
