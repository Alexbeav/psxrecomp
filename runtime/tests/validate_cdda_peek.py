"""Independent position-reader state and argument model."""
import copy,hashlib,itertools,json,subprocess,sys
from pathlib import Path
def require(ok,why):
 if not ok:raise ValueError(why)
def digest(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest()
def expected_rows(matrix):
 unowned=[-17,23,-31,4275878552,2,1,15,4,8,hashlib.sha256(bytes((i*73+19)&255 for i in range(4704))).hexdigest(),list(range(80,88))]
 for c in json.loads(Path(matrix).read_text())['cases']:
  for step,op in enumerate([dict(op='reset')]+c['operations']):
   k=op['op'];events=[];ret=None
   if k=='reset':s=dict(available=0,position_valid=0,subq=[0]*12,prefix=list(range(160,168)),suffix=list(range(192,200)),handle=0,reader=[0,0,1,1,[0]*12],unowned=copy.deepcopy(unowned))
   elif k=='stored':s.update(available=op['available'],position_valid=op['position_valid'],subq=op['values'][:])
   elif k=='handle':s['handle']=op['value']
   elif k=='reader':s['reader']=[op['return'],op['valid'],op.get('write_valid',1),op.get('write_bytes',1),op['values'][:]]
   elif k=='peek':
    events=[dict(args=[s['handle'],op['lba']&4294967295,12,1,1,0],state=copy.deepcopy(s))]
    status,valid,wv,wb,payload=s['reader'];ret=int(bool(status and valid and wv and payload[0]&15==1))
    if ret:s.update(available=1,position_valid=1,subq=payload[:])
   yield dict(case_id=c['id'],step=step-1,**{'return':ret},state=copy.deepcopy(s),events=events)
def read_check(matrix,item,commit):
 cases=json.loads(Path(matrix).read_text())['cases'];count=events=0
 with Path(item['trace']).open() as f:
  meta=json.loads(next(f))['metadata'];identity=json.loads(Path(item['identity']).read_text())
  require(meta['schema']=='t77-cdda-peek-observations-v1','schema');require(meta['source']==identity and identity['commit']==commit,'source identity')
  require(meta['matrix_sha256']==digest(matrix),'input hash');require(identity['binary_sha256']==digest(item['executable']),'binary hash')
  for a,e in itertools.zip_longest((json.loads(l) for l in f),expected_rows(matrix)):
   require(a is not None and e is not None,'row count');require(a==e,f'{e["case_id"]}:{e["step"]} full state/events');count+=1;events+=len(a['events'])
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
