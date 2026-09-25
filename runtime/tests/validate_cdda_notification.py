"""Independent full notification, callback-entry and unowned-state model."""
import copy,hashlib,json,itertools,subprocess,sys
from pathlib import Path

def require(ok,why):
 if not ok:raise ValueError(why)
def digest(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest()
def expected_rows(matrix):
 unowned=[-17,23,-31,4275878552,2,1,15,hashlib.sha256(bytes((i*73+19)&255 for i in range(4704))).hexdigest()]
 for c in json.loads(Path(matrix).read_text())['cases']:
  for step,op in enumerate([dict(op='reset')]+c['operations']):
   k=op['op'];events=[]
   if k=='reset':s=dict(notification=[0]*11,external=[0]*4,response=[0]*18,input=list(range(160,168))+[0]*8+list(range(192,200)),unowned=unowned[:])
   elif k=='input':s['input'][8:16]=op['values']
   elif k=='notification':s['notification']=[op['enabled'],op['type'],op['count']]+op['values']
   elif k=='external':s['external']=[op['irq'],op['source_clock'],op['clock'],op['ready_due']]
   elif k=='response':s['response']=[op['read'],op['count']]+op['values']
   if k in ('queue','present'):
    n=s['notification'];ex=s['external'];resp=s['response']
    if k=='queue':n[1:3]=[op['type'],op['count']];n[3:3+op['count']]=s['input'][8:8+op['count']]
    def event(kind,value=0):events.append(dict(kind=kind,value=value,state=copy.deepcopy(s)))
    if n[0] and n[1]:
     event(1)
     if ex[0]==0 and (ex[1]==0 or ex[2]>=ex[3]):
      event(2);resp[:2]=[0,0]
      for value in n[3:3+n[2]]:event(3,value);resp[2+resp[1]]=value;resp[1]+=1
      typ=n[1];n[1:3]=[0,0];event(4,typ);ex[0]=typ
      if ex[1]:ex[3]=0
      event(5)
   yield dict(case_id=c['id'],step=step-1,state=copy.deepcopy(s),events=events)
def read_check(matrix,item,commit):
 cases=json.loads(Path(matrix).read_text())['cases'];count=events=0
 with Path(item['trace']).open() as f:
  meta=json.loads(next(f))['metadata'];identity=json.loads(Path(item['identity']).read_text())
  require(meta['schema']=='t77-cdda-notification-observations-v1','schema')
  require(meta['source']==identity and identity['commit']==commit,'source identity')
  require(meta['matrix_sha256']==digest(matrix),'input hash');require(identity['binary_sha256']==digest(item['executable']),'binary hash')
  for a,e in itertools.zip_longest((json.loads(l) for l in f),expected_rows(matrix)):
   require(a is not None and e is not None,'row count')
   require(a==e,f'{e["case_id"]}:{e["step"]} full state/events')
   count+=1;events+=len(a['events'])
 require(meta['cases']==len(cases) and meta['observations']==count,'counts');return meta,dict(cases=len(cases),rows=count,events=events)
def compare(plan,job):
 observed=[]
 for side in ('baseline','candidate'):
  item=job[side]
  if not Path(item['trace']).exists():subprocess.run([sys.executable,plan['runner'],job['matrix'],item['trace'],'--executable',item['executable'],'--identity',item['identity']],capture_output=True,check=True)
  observed.append(read_check(job['matrix'],item,plan['base' if side=='baseline' else 'tested_commit']))
 a,b=observed;require(a[0]['adapter_sha256']==b[0]['adapter_sha256'],'adapter')
 for k in ('optimization','wrapper_sha256'):require(a[0]['source'][k]==b[0]['source'][k],k)
 with Path(job['baseline']['trace']).open() as l,Path(job['candidate']['trace']).open() as r:
  next(l);next(r)
  for x,y in itertools.zip_longest(l,r):require(x is not None and y is not None and json.loads(x)==json.loads(y),'paired rows')
 return dict(name=job['name'],**a[1],valid=True)
if __name__=='__main__':
 p=json.loads(Path(sys.argv[1]).read_text());results=[]
 for j in p['jobs']:
  r=compare(p,j);results.append(r);print(json.dumps(r),flush=True)
 with Path(p['result']).open('x') as f:json.dump(results,f,indent=2)
