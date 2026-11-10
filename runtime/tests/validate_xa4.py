"""Independent bounded integer evaluation, complete histories, samples and guards."""
import hashlib
import itertools
import json
import subprocess
import sys
from pathlib import Path
def require(ok,why):
    if not ok:raise ValueError(why)
def digest(path):return hashlib.sha256(Path(path).read_bytes()).hexdigest()
def seeded(seed,count,mask):return [((seed+i*73)^(i>>5))&mask if seed else 0 for i in range(count)]
def decode(data,out,hist,mono):
    a=[0,60,115,98];b=[0,0,-52,-55];hist=hist[:]
    for group in range(18):
        for unit in range(8):
            parameter=data[group*128+4+unit];shift=min(parameter&15,12);f=(parameter>>4)&3
            channel=0 if mono else unit%2;h=channel*2
            for time in range(28):
                packed=data[group*128+16+unit//2+time*4];n=(packed>>(4*(unit%2)))&15;n=n-16 if n>=8 else n
                value=(n*4096)//(1<<shift)+(hist[h]*a[f]+hist[h+1]*b[f]+32)//64
                value=max(-32768,min(32767,value));hist[h+1]=hist[h];hist[h]=value
                if mono:
                    i=((group*8+unit)*28+time)*2;out[i:i+2]=[value,value]
                else:out[((group*4+unit//2)*28+time)*2+channel]=value
    if mono:hist[2:]=hist[:2]
    return hist,4032 if mono else 2016
def expected_rows(matrix):
    for case in json.loads(Path(matrix).read_text())['cases']:
        data=[0]*2304;out=[0]*8064;hist=[0]*4
        for step,op in enumerate([dict(op='reset')]+case['operations']):
            kind=op['op'];frames=None
            if kind=='reset':data=[0]*2304;out=[0]*8064;hist=[0]*4
            elif kind=='input_seed':data=seeded(op['seed'],2304,255)
            elif kind=='output_seed':out=[x-65536 if x>=32768 else x for x in seeded(op['seed'],8064,65535)]
            elif kind=='history':hist=op['values'][:]
            elif kind=='patch':
                for offset,value in op['writes']:data[offset]=value
            elif kind in ('mono','stereo'):hist,frames=decode(data,out,hist,kind=='mono')
            yield dict(case_id=case['id'],step=step-1,frames=frames,history=hist[:],input_sha256=hashlib.sha256(bytes(data)).hexdigest(),samples=out[:],input_prefix=list(range(160,168)),input_suffix=list(range(192,200)),output_prefix=[-30000+i*173 for i in range(8)],output_suffix=[-30000+i*173 for i in range(8,16)])
def read_check(matrix,item,commit):
    cases=json.loads(Path(matrix).read_text())['cases'];count=0;decodes=0;frames=0
    with Path(item['trace']).open() as f:
        meta=json.loads(next(f))['metadata'];identity=json.loads(Path(item['identity']).read_text())
        require(meta['schema']=='t77-xa4-observations-v1','schema')
        require(meta['source']==identity and identity['commit']==commit,'source identity')
        require(meta['matrix_sha256']==digest(matrix),'input hash')
        require(meta['binary_sha256']==identity['binary_sha256']==digest(item['executable']),'binary hash')
        for actual,expected in itertools.zip_longest((json.loads(l) for l in f),expected_rows(matrix)):
            require(actual is not None and expected is not None,'row count')
            if actual!=expected:
                changed=[k for k in expected if actual.get(k)!=expected[k]]
                i=next((i for i,(x,y) in enumerate(zip(actual['samples'],expected['samples'])) if x!=y),None)
                raise ValueError(f'{expected["case_id"]}:{expected["step"]} fields {changed}; sample {i}: actual/expected '+str((actual['samples'][i],expected['samples'][i]) if i is not None else (actual['history'],expected['history'])))
            count+=1
            if actual['frames'] is not None:decodes+=1;frames+=actual['frames']
    require(meta['cases']==len(cases) and meta['observations']==count,'metadata counts')
    return meta,dict(cases=len(cases),rows=count,decodes=decodes,frames=frames)
def compare(plan,job):
    observed=[]
    for side in ('baseline','candidate'):
        item=job[side]
        if not Path(item['trace']).exists():subprocess.run([sys.executable,plan['runner'],job['matrix'],item['trace'],'--executable',item['executable'],'--identity',item['identity']],capture_output=True,check=True)
        observed.append(read_check(job['matrix'],item,plan['base' if side=='baseline' else 'tested_commit']))
    a,b=observed;require(a[0]['adapter_sha256']==b[0]['adapter_sha256'],'adapter')
    for k in ('optimization','wrapper_sha256','clamp_sha256'):require(a[0]['source'][k]==b[0]['source'][k],k)
    with Path(job['baseline']['trace']).open() as left,Path(job['candidate']['trace']).open() as right:
        next(left);next(right)
        for x,y in itertools.zip_longest(left,right):require(x is not None and y is not None and json.loads(x)==json.loads(y),'paired rows')
    return dict(name=job['name'],**a[1],valid=True)
if __name__=='__main__':
    plan=json.loads(Path(sys.argv[1]).read_text());results=[]
    for job in plan['jobs']:
        result=compare(plan,job);results.append(result);print(json.dumps(result),flush=True)
    with Path(plan['result']).open('x') as f:json.dump(results,f,indent=2)
