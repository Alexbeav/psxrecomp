"""Reject altered streaming samples, state, callbacks, identity and invalid domains."""
import copy,json,subprocess,sys,tempfile
from pathlib import Path
from validate_cdda_service import compare
p=json.loads(Path(sys.argv[1]).read_text());job=copy.deepcopy(p['jobs'][0]);results=[]
S=lambda k,v:dict(op='set',field=k,value=v)
with tempfile.TemporaryDirectory(dir=Path(__file__).resolve().parents[2]/'_scratch',prefix='cdda-service-negative-') as temp:
 root=Path(temp);matrix=root/'input.json';q=[1,1,1,0,0,0,0,0,0,16,0,0]
 ops=[S('cdda_playing',1),S('source_enabled',1),S('source_pipe_count',2),S('mode_reg',4),S('source_report_last_tens',255),dict(op='pipe',slot=0,seed=975),dict(op='raw',seed=135,fail_at=-1),dict(op='tape',records=[{'return':1,'valid':1,'values':q}]),dict(op='service',cycles=0),dict(op='raw',seed=241,fail_at=0),dict(op='service',cycles=451584)]
 matrix.write_text(json.dumps(dict(schema='t172-cdda-service-experiment-v1',cases=[dict(id='control',operations=ops)])));job['matrix']=str(matrix)
 for side in ('baseline','candidate'):job[side]['trace']=str(root/(side+'.jsonl'))
 compare(p,job);original=[json.loads(l) for l in Path(job['candidate']['trace']).read_text().splitlines()]
 def check(name,change):
  rows=copy.deepcopy(original);plan=copy.deepcopy(p);j=copy.deepcopy(job);change(rows,plan,j);out=root/(name+'.jsonl');out.write_text(''.join(json.dumps(r)+'\n' for r in rows));j['candidate']['trace']=str(out)
  try:compare(plan,j)
  except (ValueError,KeyError,IndexError):results.append(dict(name=name,rejected=True))
  else:raise AssertionError('accepted '+name)
 def event(rows,kind):return next(e for r in rows[1:] for e in r['events'] if e['kind']==kind)
 for key in ('matrix_sha256','adapter_sha256','schema'):
  check(key,lambda a,p,j,k=key:a[0]['metadata'].__setitem__(k,'wrong'))
 for key in ('commit','binary_sha256','dependencies_sha256'):
  check(key,lambda a,p,j,k=key:a[0]['metadata']['source'].__setitem__(k,'wrong'))
 check('expected-commit',lambda a,p,j:p.__setitem__('tested_commit','wrong'));check('row-count',lambda a,p,j:a.pop())
 for field in ('cdda_playing','cdda_delay','cdda_lba','cdda_sectors_played','source_sectors_read','source_pipe_at','source_pipe_count','source_report_last_tens','source_play_track_match','source_position_valid','source_async_type','source_async_count','irq_flag','stat_reg','s_source_ready_due'):
  check(field,lambda a,p,j,f=field:a[-1]['state']['fields'].__setitem__(f,a[-1]['state']['fields'][f]+1))
 for field in ('subq','async_data','response','active_volume','pending_volume','media'):
  check(field,lambda a,p,j,f=field:a[-1]['state'][f].__setitem__(0,a[-1]['state'][f][0]+1))
 check('pipe-hash',lambda a,p,j:a[-1]['state']['pipe_hashes'].__setitem__(0,'wrong'))
 check('sample',lambda a,p,j:event(a,10)['samples'].__setitem__(30,event(a,10)['samples'][30]+1))
 check('pcm-hash',lambda a,p,j:event(a,10).__setitem__('blob_sha256','wrong'))
 check('report-byte',lambda a,p,j:event(a,7)['bytes'].__setitem__(6,0))
 check('subq-argument',lambda a,p,j:event(a,5)['args'].__setitem__(1,123))
 check('raw-hash',lambda a,p,j:event(a,3).__setitem__('blob_sha256','wrong'))
 check('fatal-result',lambda a,p,j:a[-1].__setitem__('termination',0))
 check('fatal-entry',lambda a,p,j:event(a,18)['state']['fields'].__setitem__('cdda_playing',0))
 check('fatal-kind',lambda a,p,j:event(a,18).__setitem__('kind',19))
 check('event-order',lambda a,p,j:next(r for r in a[1:] if len(r['events'])>2)['events'].reverse())
 invalid=[dict(op='set',field='source_pipe_at',value=2),S('source_pipe_count',3),S('source_async_count',9),S('cdda_lba',450000),S('mode_reg',256),dict(op='service',cycles=2147483648),dict(op='pipe',slot=2,seed=1),dict(op='raw',seed=-1,fail_at=-1),dict(op='subq',values=[0]*11),dict(op='active',values=[256]*4),dict(op='response',read=16,count=0,values=[0]*16),dict(op='tape',records=[{'return':1,'valid':1,'values':[0]*12}]*129),dict(op='unknown')]
 for n,op in enumerate(invalid):
  inp=root/f'invalid-{n}.json';inp.write_text(json.dumps(dict(schema='t172-cdda-service-experiment-v1',cases=[dict(id='invalid',operations=[op])])))
  r=subprocess.run([sys.executable,p['runner'],str(inp),str(root/f'invalid-{n}.jsonl'),'--executable',job['baseline']['executable'],'--identity',job['baseline']['identity']],capture_output=True)
  assert r.returncode!=0;results.append(dict(name=f'invalid-{n}',rejected=True))
 inp=root/'underflow.json';inp.write_text(json.dumps(dict(schema='t172-cdda-service-experiment-v1',cases=[dict(id='underflow',operations=[S('cdda_playing',1),S('cdda_delay',-2147483648),dict(op='service',cycles=1)])])))
 r=subprocess.run([sys.executable,p['runner'],str(inp),str(root/'underflow.jsonl'),'--executable',job['baseline']['executable'],'--identity',job['baseline']['identity']],capture_output=True);assert r.returncode!=0;results.append(dict(name='dynamic-underflow',rejected=True))
with Path(sys.argv[2]).open('x') as f:json.dump(results,f,indent=2)
print(json.dumps(dict(rejected=len(results))))
