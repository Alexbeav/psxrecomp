"""Bind and compare complete opaque notification traces, including callback entry state."""
import hashlib
import json
import subprocess
import sys
from pathlib import Path

def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()

def require(condition, message):
    if not condition: raise ValueError(message)

def state_valid(state):
    require(set(state)=={'pending','counters','external','response'},'state fields')
    for key,size in [('pending',5),('counters',3),('external',8),('response',18)]:
        require(len(state[key])==size and all(type(x)==int and 0<=x<2**64 for x in state[key]),'state shape')
    require(state['pending'][0] in (0,1) and state['pending'][1]<256 and state['pending'][2]<8,'pending range')
    require(state['external'][0]<8 and state['external'][1]<256 and all(x<8 for x in state['external'][2:4]) and state['external'][5] in (0,1),'external range')
    require(state['response'][0]<16 and state['response'][1]<=16 and all(x<256 for x in state['response'][2:]),'response range')

def read_trace(matrix, item, commit):
    data=json.loads(Path(matrix).read_text())
    lines=[json.loads(l) for l in Path(item['trace']).read_text().splitlines()]
    meta=lines[0]['metadata']; rows=lines[1:]
    identity=json.loads(Path(item['identity']).read_text())
    require(meta['schema']=='t77-cd-pending-observations-v2','observer version')
    require(meta['source']==identity and identity['commit']==commit,'source identity')
    require(meta['matrix_sha256']==digest(matrix),'matrix digest')
    require(meta['binary_sha256']==identity['binary_sha256']==digest(item['executable']),'binary digest')
    expected=[(c['id'],step) for c in data['cases'] for step in range(-1,len(c['operations']))]
    require(meta['cases']==len(data['cases']) and meta['observations']==len(rows)==len(expected),'row count')
    require([(x['case_id'],x['step']) for x in rows]==expected,'row identity/order')
    for row in rows:
        require(set(row)=={'case_id','step','state','events'},'row fields')
        state_valid(row['state'])
        for event in row['events']:
            require(set(event)=={'kind','args','state'} and event['kind'] in range(1,9),'event kind')
            require(len(event['args'])==(4 if event['kind']==2 else 2),'event arguments')
            state_valid(event['state'])
    return meta,rows

def compare(plan, job):
    observed=[]
    for side in ('baseline','candidate'):
        item=job[side]
        if not Path(item['trace']).exists():
            subprocess.run([sys.executable,plan['runner'],job['matrix'],item['trace'],'--executable',item['executable'],'--identity',item['identity']],capture_output=True,check=True)
        observed.append(read_trace(job['matrix'],item,plan['base' if side=='baseline' else 'tested_commit']))
    require(observed[0][0]['adapter_sha256']==observed[1][0]['adapter_sha256'],'adapter')
    require(observed[0][0]['source']['wrapper_sha256']==observed[1][0]['source']['wrapper_sha256'],'wrapper')
    require(observed[0][0]['source']['optimization']==observed[1][0]['source']['optimization'],'optimization')
    for a,b in zip(observed[0][1],observed[1][1]):
        require(a==b,f"difference {a['case_id']} step {a['step']}")
    return dict(name=job['name'],cases=observed[0][0]['cases'],rows=len(observed[0][1]),callbacks=sum(len(x['events']) for x in observed[0][1]),valid=True)

if __name__=='__main__':
    plan=json.loads(Path(sys.argv[1]).read_text()); results=[]
    for job in plan['jobs']:
        result=compare(plan,job);results.append(result);print(json.dumps(result),flush=True)
    with Path(plan['result']).open('x') as f:json.dump(results,f,indent=2)
