"""Exact identity binding, integer reference arithmetic and guarded sample comparison."""
import hashlib
import json
import subprocess
import sys
from pathlib import Path
def digest(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest()
def require(ok,why):
    if not ok:raise ValueError(why)
def reference(matrix,rows):
    index=0;applications=0;frames=0
    for case in json.loads(Path(matrix).read_text())['cases']:
        pending=active=[128,0,0,128];samples=[]
        for step,op in enumerate([dict(op='reset')]+case['operations']):
            kind=op['op']
            if kind=='reset':pending=[128,0,0,128];active=pending[:];samples=[]
            elif kind=='pending':pending=op['values'][:]
            elif kind=='active':active=op['values'][:]
            elif kind=='samples':samples=op['values'][:]
            elif kind=='apply':
                applications+=1;frames+=max(0,op['frames'])
                for i in range(max(0,op['frames'])):
                    left,right=samples[i*2:i*2+2]
                    pair=[left*active[j]//128+right*active[j+2]//128 for j in range(2)]
                    samples[i*2:i*2+2]=[max(-32768,min(32767,v)) for v in pair]
            expected=dict(case_id=case['id'],step=step-1,pending=pending,active=active,samples=samples,prefix=[-30000+i*173 for i in range(8)],suffix=[-30000+i*173 for i in range(8,16)])
            require(rows[index]==expected,f'arithmetic/guard {case["id"]}:{step-1}');index+=1
    require(index==len(rows),'reference rows')
    return applications,frames
def read_trace(matrix,item,commit):
    with Path(item['trace']).open() as f:
        meta=json.loads(next(f))['metadata'];rows=[json.loads(l) for l in f]
    identity=json.loads(Path(item['identity']).read_text());cases=json.loads(Path(matrix).read_text())['cases']
    require(meta['schema']=='t77-cd-volume-observations-v1','schema')
    require(meta['source']==identity and identity['commit']==commit,'source identity')
    require(meta['matrix_sha256']==digest(matrix),'matrix hash')
    require(meta['binary_sha256']==identity['binary_sha256']==digest(item['executable']),'binary hash')
    require(meta['cases']==len(cases) and meta['observations']==len(rows),'counts')
    applications,frames=reference(matrix,rows)
    return meta,rows,applications,frames
def compare(plan,job):
    sides=[]
    for side in ('baseline','candidate'):
        item=job[side]
        if not Path(item['trace']).exists():subprocess.run([sys.executable,plan['runner'],job['matrix'],item['trace'],'--executable',item['executable'],'--identity',item['identity']],capture_output=True,check=True)
        sides.append(read_trace(job['matrix'],item,plan['base' if side=='baseline' else 'tested_commit']))
    a,b=sides
    require(a[0]['adapter_sha256']==b[0]['adapter_sha256'],'adapter')
    for field in ('optimization','wrapper_sha256'):require(a[0]['source'][field]==b[0]['source'][field],field)
    require(a[1]==b[1],'paired rows')
    return dict(name=job['name'],cases=a[0]['cases'],rows=len(a[1]),applications=a[2],frames=a[3],valid=True)
if __name__=='__main__':
    plan=json.loads(Path(sys.argv[1]).read_text());results=[]
    for job in plan['jobs']:
        result=compare(plan,job);results.append(result);print(json.dumps(result),flush=True)
    with Path(plan['result']).open('x') as f:json.dump(results,f,indent=2)
