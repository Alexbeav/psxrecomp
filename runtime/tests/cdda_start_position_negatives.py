"""Reject position evidence corruption and excluded fixture values."""
import copy,json,subprocess,sys,tempfile
from pathlib import Path
from validate_cdda_start_position import compare
p=json.loads(Path(sys.argv[1]).read_text());j=copy.deepcopy(p['jobs'][0]);results=[]
with tempfile.TemporaryDirectory(dir=Path(__file__).resolve().parents[2]/'_scratch') as temp:
 root=Path(temp);original=[json.loads(l) for l in Path(j['candidate']['trace']).read_text().splitlines()]
 changes={
 'schema':lambda a:a[0]['metadata'].__setitem__('schema','wrong'),
 'input':lambda a:a[0]['metadata'].__setitem__('matrix_sha256','wrong'),
 'source':lambda a:a[0]['metadata']['source'].__setitem__('commit','wrong'),
 'rows':lambda a:a.pop(),
 'unclamped-final':lambda a:a[-1]['state']['fields'].__setitem__('cdda_lba',2147483648),
 'unclamped-lookup':lambda a:next(e for e in a[-1]['events'] if e['kind']==5)['args'].__setitem__(0,2147483648),
 'unclamped-origin':lambda a:next(e for e in a[-1]['events'] if e['kind']==7)['args'].__setitem__(0,2147483648),
 'step':lambda a:a[-1].__setitem__('step',7)}
 for name,change in changes.items():
  a=copy.deepcopy(original);change(a);out=root/(name+'.jsonl');out.write_text(''.join(json.dumps(x)+'\n' for x in a));job=copy.deepcopy(j);job['candidate']['trace']=str(out)
  try:compare(p,job)
  except (ValueError,KeyError,IndexError):results.append(dict(name=name,rejected=True))
  else:raise AssertionError(name)
 for n,value in enumerate((-1,450000,2147483647,4294967296)):
  inp=root/f'invalid-{n}.json';inp.write_text(json.dumps(dict(schema='t172-cdda-start-position-v1',cases=[dict(id='invalid',source='track',position=value)])))
  r=subprocess.run([sys.executable,p['runner'],str(inp),str(root/f'invalid-{n}.jsonl'),'--executable',j['baseline']['executable'],'--identity',j['baseline']['identity']],capture_output=True)
  assert r.returncode!=0;results.append(dict(name=f'invalid-{n}',rejected=True))
with Path(sys.argv[2]).open('x') as f:json.dump(results,f,indent=2)
print(json.dumps(dict(rejected=len(results))))
