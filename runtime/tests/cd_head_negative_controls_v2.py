"""Reject altered head state, identity and excluded numerical domains."""
import copy,json,subprocess,sys,tempfile
from pathlib import Path
from validate_cd_head import compare
p=json.loads(Path(sys.argv[1]).read_text());job=copy.deepcopy(p['jobs'][0]);results=[]
scratch=Path(__file__).resolve().parents[2]/'_scratch';scratch.mkdir(exist_ok=True)
with tempfile.TemporaryDirectory(dir=scratch,prefix='cd-head-negative-') as temp:
 root=Path(temp);original=[json.loads(l) for l in Path(job['candidate']['trace']).read_text().splitlines()]
 def check(name,change):
  rows=copy.deepcopy(original);plan=copy.deepcopy(p);j=copy.deepcopy(job);change(rows,plan,j);out=root/(name+'.jsonl');out.write_text(''.join(json.dumps(r)+'\n' for r in rows));j['candidate']['trace']=str(out)
  try:compare(plan,j)
  except (ValueError,KeyError,IndexError):results.append(dict(name=name,rejected=True))
  else:raise AssertionError('accepted '+name)
 for key in ('schema','matrix_sha256','adapter_sha256'):check(key,lambda a,p,j,k=key:a[0]['metadata'].__setitem__(k,'wrong'))
 for key in ('commit','binary_sha256','wrapper_sha256'):check(key,lambda a,p,j,k=key:a[0]['metadata']['source'].__setitem__(k,'wrong'))
 check('expected-commit',lambda a,p,j:p.__setitem__('tested_commit','wrong'));check('row-count',lambda a,p,j:a.pop());check('case-id',lambda a,p,j:a[-1].__setitem__('case_id','wrong'))
 for key in original[-1]['state']:check(key,lambda a,p,j,k=key:a[-1]['state'].__setitem__(k,a[-1]['state'][k]+1))
 valid=dict(drive=1,valid=1,hold=0,lba=0,target=0,subq=0,mode=0,clock=0,due=0)
 changes=[dict(lba=-151),dict(lba=450000),dict(target=-2147483649),dict(target=2147483648),dict(hold=1,target=2147483646),dict(hold=-1,target=2147483647),dict(mode=256),dict(clock=-1),dict(due=2**64-451584),dict(drive=2**31),dict(valid=-2**31-1),dict(hold=2**31),dict(subq=2**31),dict(clock=451584*4096),dict(mode=128,clock=225792*4096)]
 for n,delta in enumerate(changes):
  inp=root/f'invalid-{n}.json';inp.write_text(json.dumps(dict(schema='t172-cd-head-experiment-v1',cases=[dict(id='invalid',state=dict(valid,**delta))])))
  r=subprocess.run([sys.executable,p['runner'],str(inp),str(root/f'invalid-{n}.jsonl'),'--executable',job['baseline']['executable'],'--identity',job['baseline']['identity']],capture_output=True);assert r.returncode!=0;results.append(dict(name=f'invalid-{n}',rejected=True))
with Path(sys.argv[2]).open('x') as f:json.dump(results,f,indent=2)
print(json.dumps(dict(rejected=len(results))))
