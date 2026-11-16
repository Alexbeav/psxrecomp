"""Capture actual CPU wire images and restore separately observable cache context."""
import argparse
import json
from pathlib import Path

def setv(field,value):return dict(op='set',field=field,value=value)
def capture():
    cases=[]
    for i in range(24):
        ops=[dict(op='cache_reset')]
        ops += [dict(op='absorb',index=j,value=(i*37+j*19)&255) for j in range(33)]
        ops += [setv('which',i%32),setv('pending',(i*7)%33),setv('absorb',i*12345),
                setv('fudge',i%33),setv('cycle',2**40+i*13),setv('deadline',2**40+300),
                dict(op='fetch',addr=0x80000000+i*16),setv('batch',i*3),setv('limit',64),
                setv('defer',i%3),setv('device',i%2),setv('conservative',(i//2)%2),
                setv('replay',(i//4)%2),setv('boundary_callback',1),
                setv('histogram_active',1),setv('histogram_callback',1)]
        if i%2:ops += [dict(op='local_begin')]
        ops += [setv('local',i*5)]
        cases.append(dict(id=f'capture_{i}',seed=i*7919,operations=ops))
    return dict(schema='t172-icache-experiment-v1',cases=cases)

def pairs(trace):
    source=capture();rows=[json.loads(l) for l in Path(trace).read_text().splitlines()[1:]]
    ends={r['case_id']:r for r in rows};cases=[];pairs=[]
    for i,c in enumerate(source['cases']):
        row=ends[c['id']];wire=row['cpu_wire_hex']
        restore=[dict(op='restore_cpu',cpu_wire_hex=wire,len=580),dict(op='cache_reset')]
        restore += [dict(op='tag',index=j,value=v) for j,v in enumerate(row['cache_tags']) if v!=1]
        for field,value in zip(['cache_active','histogram_active','boundary_callback','histogram_callback'],row['cache_flags']):
            restore.append(setv(field,2 if value==-1 else value))
        if row['clock'][6]:restore.append(dict(op='local_begin'))
        restore += [setv(f,v) for f,v in zip(['cycle','deadline','batch','limit','defer','local'],row['clock'][:6])]
        restore += [setv(f,v) for f,v in zip(['device','conservative','replay','load_delay','lockstep','recording','dma_wait','watch','fallback','lazy_enable'],row['flags'])]
        continuation=[dict(op='fetch_interp',addr=0x80000000+i*16),dict(op='fetch',addr=0x80001004+i*16),
                      dict(op='isolated_store',addr=i*16,control=0x804),dict(op='boundary',addr=i*16),
                      dict(op='fetch_miss',addr=0xa0000000+i*4),dict(op='local_end'),dict(op='flush')]
        left=f'left_{i}';right=f'right_{i}'
        cases += [dict(id=left,seed=c['seed'],operations=c['operations']+continuation),
                  dict(id=right,seed=c['seed'],operations=restore+continuation)]
        pairs.append(dict(left=left,right=right,left_start=len(c['operations']),right_start=len(restore),
                          length=len(continuation),source_case=c['id'],source_step=row['step'],cpu_wire_hex=wire))
    return dict(schema='t172-icache-experiment-v1',cases=cases,pairs=pairs)

p=argparse.ArgumentParser();p.add_argument('--capture-trace');a=p.parse_args()
print(json.dumps(pairs(a.capture_trace) if a.capture_trace else capture(),indent=2))
