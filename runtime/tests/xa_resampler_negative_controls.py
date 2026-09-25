"""Reject altered evidence, input identities and invalid fixture domains."""
import copy,json,subprocess,sys,tempfile
from pathlib import Path
from validate_xa_resampler import compare
plan=json.loads(Path(sys.argv[1]).read_text());job=copy.deepcopy(plan['jobs'][0]);results=[]
scratch=Path(__file__).resolve().parents[2]/'_scratch';scratch.mkdir(exist_ok=True)
with tempfile.TemporaryDirectory(prefix='xa-resampler-negative-',dir=scratch) as temp:
 work=Path(temp);matrix=work/'minimal.json'
 good=dict(id='control',samples=[32767,-32768,-32768,32767],in_frames=2,sample_rate=37800,max_frames=3,output_seed=172)
 matrix.write_text(json.dumps(dict(schema='t172-xa-resampler-experiment-v1',cases=[good])));job['matrix']=str(matrix)
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
 for field in ('input','samples','input_prefix','input_suffix','output_prefix','output_suffix'):
  check(field,lambda a,p,j,f=field:a[1][f].__setitem__(0,a[1][f][0]+1))
 check('output-tail',lambda a,p,j:a[1]['samples'].__setitem__(-1,0))
 check('frames',lambda a,p,j:a[1].__setitem__('frames',4))
 check('case-id',lambda a,p,j:a[1].__setitem__('case_id','wrong'))
 invalid=[dict(in_frames=4033),dict(in_frames=-2147483649),dict(sample_rate=44100),dict(sample_rate=1),dict(max_frames=9409),dict(max_frames=-2147483649),dict(samples=[32768,0,0,0]),dict(samples=[-32769,0,0,0]),dict(samples=[0,0]),dict(samples=[0]*8065),dict(null_in=2),dict(null_out=-1),dict(output_seed=-1),dict(output_seed=4294967296)]
 for n,changes in enumerate(invalid):
  c=dict(good,**changes);path=work/f'invalid-{n}.json';path.write_text(json.dumps(dict(schema='t172-xa-resampler-experiment-v1',cases=[c])))
  result=subprocess.run([sys.executable,plan['runner'],str(path),str(work/f'invalid-{n}.jsonl'),'--executable',job['baseline']['executable'],'--identity',job['baseline']['identity']],capture_output=True)
  if result.returncode==0:raise AssertionError('accepted invalid '+str(n))
  results.append(dict(name='invalid-'+str(n),rejected=True))
with Path(sys.argv[2]).open('x') as f:json.dump(results,f,indent=2)
print(json.dumps(dict(rejected=len(results))))
