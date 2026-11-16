"""Reject malformed texture observations and changed texels/cache work."""
import argparse
import copy
import json
import tempfile
from pathlib import Path
from validate_gpu_texture import read_trace

p=argparse.ArgumentParser();p.add_argument('matrix');p.add_argument('trace');p.add_argument('output');a=p.parse_args()
_,baseline=read_trace(a.matrix,a.trace);original=[json.loads(l) for l in Path(a.trace).read_text().splitlines()]
fetch=next(i for i,r in enumerate(original[1:],1) if r['return_value'] is not None)
sha=next(i for i,r in enumerate(original[1:],1) if r['vram_sha256'] is not None)
mutations=[]
for key,value in [('schema','wrong'),('matrix_sha256','0'*64),('cases',0),('observations',0)]:mutations.append((key,[0,'metadata',key],value))
for key,values in {'return_value':[-1,65536,True,123], 'extra_work':[-1,2**31,True,123],
                   'step':[True,999],'case_id':['wrong'],'config':[[],[2,0,0,0,0,0]], 'geometry':[[],[0,0,0,0]]}.items():
    for i,value in enumerate(values):mutations.append((f'{key}_{i}',[fetch,key],value))
mutations += [('digest',[sha,'vram_sha256'],'0'*64),('config_bool',[1,'config',0],False)]
results=[];Path('_scratch').mkdir(exist_ok=True)
with tempfile.TemporaryDirectory(dir='_scratch',prefix='texture-negative-') as temp:
    target=Path(temp)/'trace.jsonl'
    for name,path,value in mutations:
        data=copy.deepcopy(original);parent=data
        for key in path[:-1]:parent=parent[key]
        parent[path[-1]]=value
        target.write_text('\n'.join(json.dumps(r) for r in data)+'\n')
        try:
            _,rows=read_trace(a.matrix,target)
            if rows==baseline:raise RuntimeError('corruption escaped: '+name)
            rejected='comparison'
        except ValueError:rejected='structure'
        results.append(dict(name=name,rejected_by=rejected))
    for name,data in [('missing',original[:-1]),('extra',original+[original[-1]])]:
        target.write_text('\n'.join(json.dumps(r) for r in data)+'\n')
        try:read_trace(a.matrix,target)
        except ValueError:results.append(dict(name=name,rejected_by='structure'))
        else:raise RuntimeError('corruption escaped: '+name)
Path(a.output).write_text(json.dumps(results,indent=2));print(json.dumps(dict(rejected=len(results))))
