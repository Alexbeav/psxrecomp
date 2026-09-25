"""Reject damaged opcode traces, including validly shaped output substitutions."""
import argparse
import copy
import json
import tempfile
from pathlib import Path
from validate_instr_cost import compare

p=argparse.ArgumentParser();p.add_argument('matrix');p.add_argument('trace');p.add_argument('output');a=p.parse_args()
original=[json.loads(line) for line in Path(a.trace).read_text().splitlines()]
mutations=[]
for key,value in [('schema','wrong'),('matrix_sha256','0'*64),('cases',0),('observations',0)]:
    mutations.append((key,[0,'metadata',key],value))
for key,values in {'word':[-1,2**32,True,123], 'base_cycles':[-1,2**32,True,123],
                   'dependency_mask':[-1,2**32,True,123],'index':[True,19],'case_id':['wrong']}.items():
    for i,value in enumerate(values):mutations.append((f'{key}_{i}',[1,key],value))
results=[];Path('_scratch').mkdir(exist_ok=True)
with tempfile.TemporaryDirectory(dir='_scratch',prefix='opcode-negative-') as temp:
    target=Path(temp)/'trace.jsonl'
    for name,path,value in mutations:
        data=copy.deepcopy(original);parent=data
        for key in path[:-1]:parent=parent[key]
        parent[path[-1]]=value
        target.write_text('\n'.join(json.dumps(row) for row in data)+'\n')
        try:compare(a.matrix,a.trace,target)
        except ValueError as error:results.append(dict(name=name,rejected=True,reason=str(error)))
        else:raise RuntimeError('corruption escaped: '+name)
    for name,data in [('missing',original[:-1]),('extra',original+[original[-1]])]:
        target.write_text('\n'.join(json.dumps(row) for row in data)+'\n')
        try:compare(a.matrix,a.trace,target)
        except ValueError as error:results.append(dict(name=name,rejected=True,reason=str(error)))
        else:raise RuntimeError('corruption escaped: '+name)
Path(a.output).write_text(json.dumps(results,indent=2));print(json.dumps(dict(rejected=len(results))))
