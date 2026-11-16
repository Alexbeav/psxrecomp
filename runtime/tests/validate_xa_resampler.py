"""Independent integer evaluation of pinned resampler observations."""
import hashlib,itertools,json,subprocess,sys
from pathlib import Path
def require(ok,why):
 if not ok:raise ValueError(why)
def digest(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest()
def signed(v,bits):return (v+(1<<(bits-1)))%(1<<bits)-(1<<(bits-1))
def trunc(n,d):return n//d if n>=0 else -((-n)//d)
def expected(c):
 data=c['samples']+[0]*(8064-len(c['samples']));seed=c.get('output_seed',0)
 out=[signed(((seed+i*73)^(i>>5))&65535,16) if seed else 0 for i in range(18816)]
 n=c['in_frames'];rate=c['sample_rate'];cap=c['max_frames'];count=0
 if n>0 and rate>0 and cap>0 and not c.get('null_in',0) and not c.get('null_out',0):
  count=min(cap,(n*44100+rate-1)//rate)
  for i in range(count):
   pos,rem=divmod(i*rate,44100);nextpos=min(pos+1,n-1)
   for ch in range(2):
    a=data[2*pos+ch];b=data[2*nextpos+ch]
    out[2*i+ch]=signed(a+trunc(signed((b-a)*rem,32),44100),16)
 return dict(case_id=c['id'],frames=count,input=data,samples=out,input_prefix=[-28000+i*137 for i in range(8)],input_suffix=[-26000+i*137 for i in range(8)],output_prefix=[-24000+i*137 for i in range(8)],output_suffix=[-22000+i*137 for i in range(8)])
def read_check(matrix,item,commit):
 cases=json.loads(Path(matrix).read_text())['cases'];count=0;frames=0
 with Path(item['trace']).open() as f:
  meta=json.loads(next(f))['metadata'];identity=json.loads(Path(item['identity']).read_text())
  require(meta['schema']=='t77-xa-resampler-observations-v1','schema')
  require(meta['source']==identity and identity['commit']==commit,'source identity')
  require(meta['matrix_sha256']==digest(matrix),'input hash')
  require(identity['binary_sha256']==digest(item['executable']),'binary hash')
  for actual,c in itertools.zip_longest((json.loads(l) for l in f),cases):
   require(actual is not None and c is not None,'row count');e=expected(c)
   if actual!=e:
    keys=[k for k in set(actual)|set(e) if actual.get(k)!=e.get(k)]
    i=next((i for i,(x,y) in enumerate(zip(actual['samples'],e['samples'])) if x!=y),None)
    raise ValueError(f'{c["id"]}: {keys}; sample {i}: '+str((actual['samples'][i],e['samples'][i]) if i is not None else (actual['frames'],e['frames'])))
   count+=1;frames+=actual['frames']
 require(meta['cases']==count,'metadata counts');return meta,dict(cases=count,rows=count,frames=frames)
def compare(plan,job):
 observed=[]
 for side in ('baseline','candidate'):
  item=job[side]
  if not Path(item['trace']).exists():subprocess.run([sys.executable,plan['runner'],job['matrix'],item['trace'],'--executable',item['executable'],'--identity',item['identity']],capture_output=True,check=True)
  observed.append(read_check(job['matrix'],item,plan['base' if side=='baseline' else 'tested_commit']))
 a,b=observed;require(a[0]['adapter_sha256']==b[0]['adapter_sha256'],'adapter')
 for k in ('optimization','wrapper_sha256','sha_source_sha256'):require(a[0]['source'][k]==b[0]['source'][k],k)
 with Path(job['baseline']['trace']).open() as l,Path(job['candidate']['trace']).open() as r:
  next(l);next(r)
  for x,y in itertools.zip_longest(l,r):require(x is not None and y is not None and json.loads(x)==json.loads(y),'paired rows')
 return dict(name=job['name'],**a[1],valid=True)
if __name__=='__main__':
 p=json.loads(Path(sys.argv[1]).read_text());results=[]
 for j in p['jobs']:
  r=compare(p,j);results.append(r);print(json.dumps(r),flush=True)
 with Path(p['result']).open('x') as f:json.dump(results,f,indent=2)
