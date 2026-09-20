"""Paired continuations from actual baseline CPU wire images and reseeded context."""
import argparse
import json
from pathlib import Path
from validate_cpu_timing_trace import read_trace


def restore(matrix_path, trace_path):
    original=json.loads(Path(matrix_path).read_text(encoding='utf-8-sig'))
    _,rows=read_trace(matrix_path,trace_path)
    last={r['case_id']:r for r in rows}
    cases=[];pairs=[]
    future=[dict(op='word_slow',addr=0x80000000,rt=3,mask=8),
            dict(op='half_slow',addr=0xbf801070,rt=7,mask=128),
            dict(op='byte',addr=0x1f801803,rt=31,mask=2**31),
            dict(op='lwc2',addr=0x1f801800),dict(op='step',mask=0xffffffff),
            dict(op='timing_only',addr=0x1f801c00,rt=0,mask=0),
            dict(op='byte',addr=0x1fffff,rt=1,mask=2),
            dict(op='begin'),dict(op='local_begin'),dict(op='charge',cycles=63),
            dict(op='word_slow',addr=0x1f801100,rt=3,mask=0),
            dict(op='local_end'),dict(op='end'),dict(op='flush')]
    for case in original['cases'][::6]:
        row=last[case['id']]
        if row['clock'][6]:raise ValueError('checkpoint has live local pointer')
        state=dict(zip(['cycle','deadline','batch','limit','defer','local'],row['clock'][:6]))
        state.update(zip(['device','conservative','replay','load_delay','lockstep','recording',
                          'dma_wait','watch','fallback','lazy_enable'],row['flags']))
        if state['load_delay']==-1:state['load_delay']=2
        state.update(zip(['source_profile','hblank_sample','clock_value'],row['memory_flags']))
        if 'memory_extra' in row:state.update(zip(['fast_limit','dma_depth'],row['memory_extra']))
        restored=[dict(op='restore_cpu',cpu_wire_hex=row['cpu_wire_hex'],len=580)]
        restored += [dict(op='set',field=k,value=v) for k,v in state.items()]
        left,right=case['id']+'_continued',case['id']+'_restored'
        cases += [dict(id=left,seed=case.get('seed',0),operations=case['operations']+future),
                  dict(id=right,seed=case.get('seed',0),operations=restored+future)]
        pairs.append(dict(left=left,right=right,left_start=len(case['operations']),
                          right_start=len(restored),length=len(future),
                          source_case=case['id'],source_step=row['step'],cpu_wire_hex=row['cpu_wire_hex']))
    return dict(schema=original['schema'],cases=cases,pairs=pairs)


if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('matrix');p.add_argument('trace');p.add_argument('output',type=Path)
    a=p.parse_args()
    with a.output.open('x') as f:json.dump(restore(a.matrix,a.trace),f,indent=2)
