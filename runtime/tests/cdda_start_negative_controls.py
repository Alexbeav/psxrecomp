"""Reject changed partial fatal states, ordering and fixture-domain violations."""
import copy,json,subprocess,sys,tempfile
from pathlib import Path
from validate_cdda_start import compare
plan=json.loads(Path(sys.argv[1]).read_text());job=copy.deepcopy(plan['jobs'][0]);results=[]
scratch=Path(__file__).resolve().parents[2]/'_scratch';scratch.mkdir(exist_ok=True)
with tempfile.TemporaryDirectory(prefix='cdda-start-negative-',dir=scratch) as temp:
 work=Path(temp);matrix=work/'minimal.json'
 fixture=dict(track_count=1,start_lba=123,selected_track=1,audio=1,delay=2147483647,jitter=1,peek_at=31,values=list(range(12)))
 good=dict(id='control',operations=[dict(op='start',requested_track=1),dict(op='fixture',**fixture),dict(op='start',requested_track=1)])
 matrix.write_text(json.dumps(dict(schema='t172-cdda-start-experiment-v1',cases=[good])));job['matrix']=str(matrix)
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
 check('expected-source',lambda a,p,j:p.__setitem__('tested_commit','0'*40));check('row-count',lambda a,p,j:a.pop())
 check('schema',lambda a,p,j:a[0]['metadata'].__setitem__('schema','wrong'))
 for field in ('source_enabled','source_seeking','source_play_track_match','source_async_type','cdda_delay','cdda_lba','cdda_sectors_played','stat_reg','setloc_pending','pending_dataready_slot'):
  check(field,lambda a,p,j,f=field:a[-1]['state']['fields'].__setitem__(f,a[-1]['state']['fields'][f]+1))
 for field in ('subq','async_data','counters'):
  check(field,lambda a,p,j,f=field:a[-1]['state'][f].__setitem__(0,a[-1]['state'][f][0]+1))
 check('pipe',lambda a,p,j:a[-1]['state'].__setitem__('pipe_sha256','0'*64))
 check('fatal-result',lambda a,p,j:a[-1].__setitem__('termination',0))
 check('fatal-entry-mutation',lambda a,p,j:a[-1]['events'][-1]['state']['fields'].__setitem__('cdda_delay',0))
 check('fatal-kind',lambda a,p,j:a[-1]['events'][-1].__setitem__('kind',13))
 check('order',lambda a,p,j:a[2]['events'].__setitem__(slice(0,2),a[2]['events'][:2][::-1]))
 check('probe-bound',lambda a,p,j:a[2]['events'].pop(-2))
 check('seek-args',lambda a,p,j:next(e for e in a[2]['events'] if e['kind']==7)['args'].__setitem__(0,123))
 check('case-id',lambda a,p,j:a[-1].__setitem__('case_id','wrong'))
 invalid=[dict(op='start',requested_track=2147483648),dict(op='set',field='mode_reg',value=256),dict(op='set',field='read_min',value=-1),dict(op='set',field='source_pipe_count',value=-1),dict(op='set',field='cdda_lba',value=450000),dict(op='handle',value=3),dict(op='fixture',**dict(fixture,jitter=25000)),dict(op='fixture',**dict(fixture,delay=-1)),dict(op='fixture',**dict(fixture,peek_at=32)),dict(op='fixture',**dict(fixture,values=[0]*11)),dict(op='unknown')]
 for n,op in enumerate(invalid):
  path=work/f'invalid-{n}.json';path.write_text(json.dumps(dict(schema='t172-cdda-start-experiment-v1',cases=[dict(id='invalid',operations=[op])])))
  result=subprocess.run([sys.executable,plan['runner'],str(path),str(work/f'invalid-{n}.jsonl'),'--executable',job['baseline']['executable'],'--identity',job['baseline']['identity']],capture_output=True)
  if result.returncode==0:raise AssertionError('accepted invalid '+str(n))
  results.append(dict(name='invalid-'+str(n),rejected=True))
with Path(sys.argv[2]).open('x') as f:json.dump(results,f,indent=2)
print(json.dumps(dict(rejected=len(results))))
