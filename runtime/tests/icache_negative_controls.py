"""Prove malformed and changed cache observations cannot pass qualification."""
import argparse
import copy
import json
import tempfile
from pathlib import Path
from validate_cpu_timing_trace import read_trace

p=argparse.ArgumentParser();p.add_argument('matrix');p.add_argument('trace');p.add_argument('output');a=p.parse_args()
meta,baseline=read_trace(a.matrix,a.trace)
original=[json.loads(line) for line in Path(a.trace).read_text().splitlines()]
event_index=next(i for i,r in enumerate(original[1:],1) if r['events'])
boundary_index=next(i for i,r in enumerate(original[1:],1) if any(e[0]==13 for e in r['events']))
mutations=[]
def add(name,path,value):mutations.append((name,path,value))
for key,value in [('schema','wrong'),('matrix_sha256','0'*64),('cases',0),('observations',0)]:
    add('metadata_'+key,[0,'metadata',key],value)
for key,value in [('cache_flags',[]),('cache_tags',[]),('event_cache_tags',[]),('cpu_wire_hex','00'),
                  ('event_cpu_states',[]),('events',[[]]),('case_id','wrong'),('step',True)]:
    add('shape_'+key,[event_index,key],value)
add('cache_flag_range',[1,'cache_flags',0],3)
add('cache_flag_bool',[1,'cache_flags',0],True)
add('tag_negative',[1,'cache_tags',0],-1)
add('tag_overflow',[1,'cache_tags',0],2**32)
add('event_tag_overflow',[event_index,'event_cache_tags',0,0],2**32)
add('event_kind',[event_index,'events',0,0],15)
add('boundary_cycle_overflow',[boundary_index,'events',0,2],2**64)
add('boundary_pointer_range',[boundary_index,'events',0,4],2)
add('changed_tag',[1,'cache_tags',0],123)
add('changed_cache_active',[1,'cache_flags',0],1)
add('changed_cpu_wire',[1,'cpu_wire_hex'],'01'+baseline[0]['cpu_wire_hex'][2:])
add('changed_clock',[1,'clock',0],17)
add('changed_callback_tag',[event_index,'event_cache_tags',0,0],123)
add('changed_callback_cpu',[event_index,'event_cpu_states',0],'01'+original[event_index]['event_cpu_states'][0][2:])
add('changed_callback_cycle',[boundary_index,'events',0,2],123)
results=[]
scratch=Path('_scratch');scratch.mkdir(exist_ok=True)
with tempfile.TemporaryDirectory(dir=scratch,prefix='icache-negative-') as temp:
    target=Path(temp)/'trace.jsonl'
    for name,path,value in mutations:
        data=copy.deepcopy(original);parent=data
        for key in path[:-1]:parent=parent[key]
        parent[path[-1]]=value
        target.write_text('\n'.join(json.dumps(r) for r in data)+'\n')
        try:
            _,rows=read_trace(a.matrix,target)
            if rows==baseline:raise RuntimeError(f'corruption escaped: {name}')
            rejection='comparison'
        except ValueError:
            rejection='structure'
        results.append(dict(name=name,rejected_by=rejection))
Path(a.output).write_text(json.dumps(results,indent=2))
print(json.dumps(dict(attempts=len(results),rejected=len(results))))
