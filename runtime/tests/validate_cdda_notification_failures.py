"""Bind oversized-count abort boundary and reject corrupted failure evidence."""
import copy,hashlib,json,subprocess,sys
from pathlib import Path
from validate_cdda_notification import require,digest
E=Path(sys.argv[1]);R=Path(sys.argv[2]);matrix=E/'cdda-notification-failures-v2.json';inputs=json.loads(matrix.read_text())['cases'];results=[];negatives=[]
base='91ca382196ac9c74fa3da726ed0797746b557982';tested='e525e716659a479cf82d37fcf6878fc2d5edbd63'
def check(p,identity,exe,commit):
 require(p['schema']=='t77-cdda-notification-failures-v1','schema');require(p['matrix_sha256']==digest(matrix),'matrix')
 require(p['source']==identity and identity['commit']==commit and identity['capture_abort'] is True,'identity')
 require(identity['binary_sha256']==digest(exe),'binary');require(len(p['cases'])==len(inputs),'cases');require(p['all_rejected_before_mutation'] is True,'summary')
 for actual,c in zip(p['cases'],inputs):
  require(actual['case_id']==c['id'] and actual['count']==c['count'] and actual['type']==c['type'],'case input')
  b=actual['before'];expected=dict(notification=[1,4,3]+list(range(80,88)),external=[1,1,99,100],response=[15,16]+list(range(160,176)),unowned=[-17,23,-31,4275878552,2,1,15,hashlib.sha256(bytes((i*73+19)&255 for i in range(4704))).hexdigest()],input=list(range(160,168))+c['payload']+list(range(192,200)))
  require(b==expected,'fixture');require(actual['exit_code']==99,'exit')
  require(actual['rejection']==dict(events=[dict(kind=6,value=0,state=b)],aborted=True,state=b),'abort before mutation/callback')
for opt in ('O0','O2'):
 pair=[]
 for side,sha,label,commit in [('baseline','91ca382196ac','guard-v2',base),('candidate','e525e716659a','t172-v2',tested)]:
  stem=f'cdda-notification-candidate-{sha}-{opt}-{label}-abort';exe=R/(stem+'.exe');ip=R/(stem+'-receipt.json');out=E/f'cdda-notification-failures-{side}-{opt}-v2.jsonl'
  if not out.exists():subprocess.run([sys.executable,str(R/'run_cdda_notification_failure_matrix.py'),str(matrix),str(out),'--executable',str(exe),'--identity',str(ip)],capture_output=True,check=True)
  p=json.loads(out.read_text());identity=json.loads(ip.read_text());check(p,identity,exe,commit);pair.append(p)
  if side=='candidate' and opt=='O0':
   changes=[('exit',lambda x:x['cases'][0].__setitem__('exit_code',0)),('count',lambda x:x['cases'][0].__setitem__('count',8)),('mutation',lambda x:x['cases'][0]['rejection']['state']['notification'].__setitem__(1,0)),('extra-callback',lambda x:x['cases'][0]['rejection']['events'].append(dict(kind=1,value=0,state=x['cases'][0]['before']))),('abort-kind',lambda x:x['cases'][0]['rejection']['events'][0].__setitem__('kind',1)),('input',lambda x:x['cases'][0]['before']['input'].__setitem__(8,0)),('matrix',lambda x:x.__setitem__('matrix_sha256','0'*64)),('identity',lambda x:x['source'].__setitem__('commit','0'*40))]
   for name,change in changes:
    bad=copy.deepcopy(p);change(bad)
    try:check(bad,identity,exe,commit)
    except ValueError:negatives.append(dict(name=name,rejected=True))
    else:raise AssertionError('accepted '+name)
 require(pair[0]['cases']==pair[1]['cases'],'paired failures');require(pair[0]['adapter_sha256']==pair[1]['adapter_sha256'],'adapter')
 require(pair[0]['source']['wrapper_sha256']==pair[1]['source']['wrapper_sha256'],'wrapper')
 results.append(dict(optimization=opt,cases=len(inputs),rejected_before_mutation=True))
with (E/'cdda-notification-failure-results-v2.json').open('x') as f:json.dump(dict(results=results,negative_controls=negatives),f,indent=2)
print(json.dumps(dict(failure_cases=sum(x['cases'] for x in results),negative_controls=len(negatives))))
