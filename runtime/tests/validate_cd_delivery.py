"""Compare complete traces and independently rebuild synthetic payloads and tails."""
import hashlib
import json
import subprocess
import sys
from pathlib import Path
def require(ok,why):
    if not ok:raise ValueError(why)
def digest(path):return hashlib.sha256(Path(path).read_bytes()).hexdigest()
def seed_bytes(seed,start,length):
    return bytearray(((seed+x*73)^(x>>5))&255 for x in range(start,start+length)) if seed else bytearray(length)
def payload_check(matrix,rows):
    cases=json.loads(Path(matrix).read_text())['cases']; index=0; dumps=0
    for case in cases:
        slots=[seed_bytes(case.get('seed',0),i*2340,2340) for i in range(8)]
        media=dict(delivered=0,have_raw=0,user_seed=0,raw_seed=0)
        previous=rows[index]['state'];index+=1
        for step,op in enumerate(case['operations']):
            row=rows[index];index+=1;kind=op['op']
            if kind=='reset':
                slots=[seed_bytes(op.get('seed',0),i*2340,2340) for i in range(8)];media=dict(delivered=0,have_raw=0,user_seed=0,raw_seed=0)
            elif kind=='slot':slots[op['slot']]=seed_bytes(op['seed'],op['slot']*2340,2340)
            elif kind=='media':media=op
            elif kind in ('fill','immediate','without_irq') and media['delivered']:
                target=(previous['external'][1]+1)%8; user=seed_bytes(media['user_seed'],0,2048)
                if not (previous['external'][6]&32):data=user
                elif media['have_raw']:data=seed_bytes(media['raw_seed'],0,2352)[12:]
                else:
                    coords=previous['external'][2:5]
                    data=bytearray([(v//10*16+v%10)&255 for v in coords]+[2]+[0]*8)+user
                slots[target]=data+bytearray(2340-len(data))
            for i,slot in enumerate(slots):require(hashlib.sha256(slot).hexdigest()==row['state']['slots'][i][2],f'payload hash {case["id"]}:{step}:{i}')
            if kind=='dump':
                require(bytes.fromhex(row['payload'])==slots[op['slot']],f'payload bytes {case["id"]}:{step}');dumps+=1
            else:require(row['payload'] is None,'unexpected payload')
            previous=row['state']
    require(index==len(rows),'payload row count')
    return dumps
def read_trace(matrix,item,commit):
    with Path(item['trace']).open() as f:
        meta=json.loads(next(f))['metadata'];rows=[json.loads(l) for l in f]
    identity=json.loads(Path(item['identity']).read_text());cases=json.loads(Path(matrix).read_text())['cases']
    require(meta['source']==identity and identity['commit']==commit,'source identity')
    require(meta['matrix_sha256']==digest(matrix),'matrix hash')
    require(meta['binary_sha256']==identity['binary_sha256']==digest(item['executable']),'binary hash')
    require(meta['schema']=='t77-cd-delivery-observations-v1','observer schema')
    expected=[(c['id'],s) for c in cases for s in range(-1,len(c['operations']))]
    require(meta['cases']==len(cases) and meta['observations']==len(rows)==len(expected),'row count')
    require([(r['case_id'],r['step']) for r in rows]==expected,'row identity/order')
    for row in rows:
        require(set(row)=={'case_id','step','events','state','return_value','payload'},'row fields')
        for event in row['events']:require(event['kind'] in range(1,9) and len(event['args'])==3,'callback shape')
    return meta,rows,payload_check(matrix,rows)
def compare(plan,job):
    sides=[]
    for side in ('baseline','candidate'):
        item=job[side]
        if not Path(item['trace']).exists():subprocess.run([sys.executable,plan['runner'],job['matrix'],item['trace'],'--executable',item['executable'],'--identity',item['identity']],capture_output=True,check=True)
        sides.append(read_trace(job['matrix'],item,plan['base' if side=='baseline' else 'tested_commit']))
    a,b=sides
    for field in ('adapter_sha256',):require(a[0][field]==b[0][field],field)
    for field in ('optimization','wrapper_sha256'):require(a[0]['source'][field]==b[0]['source'][field],field)
    for x,y in zip(a[1],b[1]):require(x==y,f'difference {x["case_id"]}:{x["step"]}')
    return dict(name=job['name'],cases=a[0]['cases'],rows=len(a[1]),callbacks=sum(len(r['events']) for r in a[1]),payloads=a[2],valid=True)
if __name__=='__main__':
    p=json.loads(Path(sys.argv[1]).read_text());results=[]
    for job in p['jobs']:
        r=compare(p,job);results.append(r);print(json.dumps(r),flush=True)
    with Path(p['result']).open('x') as f:json.dump(results,f,indent=2)
