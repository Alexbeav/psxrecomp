"""Validate texture observations and independently reproduce synthetic VRAM hashes."""
import array
import functools
import hashlib
import json
import struct
import sys
from pathlib import Path

def require(value,message):
    if not value:raise ValueError(message)
def digest(path):
    with Path(path).open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
def integer(value,maximum):return type(value) is int and 0<=value<=maximum
@functools.lru_cache(maxsize=4)
def initial_vram(seed):
    values=array.array('H',(((seed+i*73)^(i>>5))&65535 for i in range(524288))) if seed else array.array('H',[0])*524288
    if sys.byteorder!='little':values.byteswap()
    return values.tobytes()

def read_trace(matrix_path,trace_path,identity=None):
    matrix=json.loads(Path(matrix_path).read_text(encoding='utf-8-sig'))
    require(matrix['schema']=='t172-texture-experiment-v1','matrix schema')
    require(len({c['id'] for c in matrix['cases']})==len(matrix['cases']),'duplicate case')
    with Path(trace_path).open() as source:
        metadata=json.loads(next(source))['metadata']
        require(metadata['schema']=='t77-texture-observations-v1','trace schema')
        require(metadata['matrix_sha256']==digest(matrix_path),'matrix hash')
        require(metadata['cases']==len(matrix['cases']),'case count')
        require(metadata['observations']==sum(1+len(c['operations']) for c in matrix['cases']),'row count')
        if identity:require(metadata['source']==json.loads(Path(identity).read_text()),'source identity')
        rows=[]
        for case in matrix['cases']:
            seed=case.get('seed',0);memory=None;writes={};config=[0]*6
            needs_memory=any(op['op']=='digest' for op in case['operations'])
            for step in range(-1,len(case['operations'])):
                line=next(source,None);require(line is not None,'missing row');row=json.loads(line)
                require(set(row)=={'case_id','step','return_value','extra_work','config','geometry','vram_sha256'},'row keys')
                require(row['case_id']==case['id'] and type(row['step']) is int and row['step']==step,'row order')
                op=case['operations'][step] if step>=0 else dict(op='reset',seed=seed)
                name=op['op']
                if name=='reset':
                    seed=op.get('seed',0);writes={};config=[0]*6
                    memory=bytearray(initial_vram(seed)) if needs_memory else None
                elif name=='set' and op['field'] in ['mode','page','clut','window','raw','load_clut']:
                    config[['mode','page','clut','window','raw','load_clut'].index(op['field'])]=op['value']
                elif name=='write':
                    writes[op['index']]=op['value']
                    if memory is not None:struct.pack_into('<H',memory,2*op['index'],op['value'])
                require(row['config']==config and all(type(v) is int for v in row['config']),'config')
                require(integer(row['extra_work'],2**31-1),'extra work')
                if name in ['fetch','read']:require(integer(row['return_value'],65535),'return range')
                else:require(row['return_value'] is None,'unexpected return')
                if name=='read':
                    i=op['index'];expected=writes.get(i,((seed+i*73)^(i>>5))&65535 if seed else 0)
                    require(row['return_value']==expected,'VRAM read changed')
                if name=='sprite':
                    geometry=row['geometry'];require(type(geometry) is list and len(geometry)==4,'geometry shape')
                    require(all(integer(v,limit) for v,limit in zip(geometry,[1,3,1023,511])),'geometry range')
                else:require(row['geometry'] is None,'unexpected geometry')
                if name=='digest':require(row['vram_sha256']==hashlib.sha256(memory).hexdigest(),'VRAM changed')
                else:require(row['vram_sha256'] is None,'unexpected digest')
                rows.append(row)
        require(next(source,None) is None,'extra row')
    return metadata,rows
