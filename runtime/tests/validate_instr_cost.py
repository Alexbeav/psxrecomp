"""Strict streaming checks of raw opcode observations against their input matrix."""
import argparse
import hashlib
import json
from pathlib import Path

def require(value,message):
    if not value:raise ValueError(message)
def digest(path):
    with Path(path).open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()

def trace(matrix_path,trace_path,identity=None):
    matrix=json.loads(Path(matrix_path).read_text(encoding='utf-8-sig'))
    require(matrix['schema']=='t172-instr-cost-experiment-v1','matrix schema')
    require(len({c['id'] for c in matrix['cases']})==len(matrix['cases']),'duplicate case')
    with Path(trace_path).open() as source:
        meta=json.loads(next(source))['metadata']
        require(meta['schema']=='t77-instr-cost-observations-v1','trace schema')
        require(meta['matrix_sha256']==digest(matrix_path),'matrix hash')
        require(meta['cases']==len(matrix['cases']),'case count')
        require(meta['observations']==sum(len(c['words']) for c in matrix['cases']),'row count')
        if identity:require(meta['source']==json.loads(Path(identity).read_text()),'source identity')
        for case in matrix['cases']:
            for index,word in enumerate(case['words']):
                line=next(source,None);require(line is not None,'missing row')
                row=json.loads(line)
                require(set(row)=={'case_id','index','word','base_cycles','dependency_mask'},'row keys')
                require(type(row['index']) is int and row['index']==index and row['case_id']==case['id'],'row order')
                for key in ['word','base_cycles','dependency_mask']:
                    require(type(row[key]) is int and 0<=row[key]<=0xffffffff,'u32 '+key)
                require(row['word']==word,'word changed')
                yield row
        require(next(source,None) is None,'extra row')

def compare(matrix,a,b,aid=None,bid=None):
    count=0
    # Both generators share the exact input count and reject missing/extra rows.
    ga,gb=trace(matrix,a,aid),trace(matrix,b,bid)
    while True:
        x=next(ga,None);y=next(gb,None)
        if x is None and y is None:break
        require(x==y,f'difference at row {count}')
        count+=1
    return count

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('matrix');p.add_argument('baseline');p.add_argument('candidate')
    a=p.parse_args();print(json.dumps(dict(rows=compare(a.matrix,a.baseline,a.candidate),valid=True)))
