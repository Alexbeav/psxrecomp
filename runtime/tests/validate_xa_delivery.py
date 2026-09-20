"""Complete event/state/sample comparison with source identity binding."""
import hashlib,itertools,json,subprocess,sys
from pathlib import Path
from xa_delivery_model import expected_rows
from validate_xa_resampler import require,digest

def read_check(matrix,item,commit):
 cases=json.loads(Path(matrix).read_text())['cases'];count=events=frames=0
 with Path(item['trace']).open() as f:
  meta=json.loads(next(f))['metadata'];identity=json.loads(Path(item['identity']).read_text())
  require(meta['schema']=='t77-xa-delivery-observations-v1','schema')
  require(meta['source']==identity and identity['commit']==commit,'source identity')
  require(meta['matrix_sha256']==digest(matrix),'input hash')
  require(identity['binary_sha256']==digest(item['executable']),'binary hash')
  for actual,e in itertools.zip_longest((json.loads(l) for l in f),expected_rows(matrix)):
   require(actual is not None and e is not None,'row count')
   if actual!=e:
    keys=[k for k in set(actual)|set(e) if actual.get(k)!=e.get(k)]
    i=next((i for i,(x,y) in enumerate(zip(actual['events'],e['events'])) if x!=y),None)
    raise ValueError(f'{e["case_id"]}:{e["step"]} fields {keys}; event {i}')
   count+=1;events+=len(actual['events']);frames+=sum(x['args'][0] for x in actual['events'] if x['kind']==11)
 require(meta['cases']==len(cases) and meta['observations']==count,'metadata counts');return meta,dict(cases=len(cases),rows=count,events=events,frames=frames)
def compare(plan,job):
 observed=[]
 for side in ('baseline','candidate'):
  item=job[side]
  if not Path(item['trace']).exists():subprocess.run([sys.executable,plan['runner'],job['matrix'],item['trace'],'--executable',item['executable'],'--identity',item['identity']],capture_output=True,check=True)
  observed.append(read_check(job['matrix'],item,plan['base' if side=='baseline' else 'tested_commit']))
 a,b=observed;require(a[0]['adapter_sha256']==b[0]['adapter_sha256'],'adapter')
 for k in ('optimization','wrapper_sha256','dependencies_sha256'):require(a[0]['source'][k]==b[0]['source'][k],k)
 with Path(job['baseline']['trace']).open() as l,Path(job['candidate']['trace']).open() as r:
  next(l);next(r)
  for x,y in itertools.zip_longest(l,r):require(x is not None and y is not None and json.loads(x)==json.loads(y),'paired rows')
 return dict(name=job['name'],**a[1],valid=True)
if __name__=='__main__':
 p=json.loads(Path(sys.argv[1]).read_text());results=[]
 for j in p['jobs']:
  r=compare(p,j);results.append(r);print(json.dumps(r),flush=True)
 with Path(p['result']).open('x') as f:json.dump(results,f,indent=2)
