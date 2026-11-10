"""Independently authored synthetic pending-notification observations."""
import argparse
import json
import random
from pathlib import Path

def setv(field, value):
    return dict(op='set', field=field, value=value)

def generate(seed):
    cases=[]
    def add(ops):
        cases.append(dict(id=f'cd-{seed}-{len(cases)}', operations=ops))
    for profile in (0,1):
        for irq in range(8):
            for delivered in (0,1):
                for slot in (0,7):
                    for stat in (0,255):
                        setup=[setv('s_source_clock',profile),setv('irq_flag',irq),setv('s_ring_write',slot),setv('stat_reg',stat),setv('psx_cycle_count',1000),setv('s_source_ready_due',2000),dict(op='response',read=3,count=8,bytes=list(range(16))),dict(op='arrive',delivered=delivered,sequence=55)]
                        for tail in ([dict(op='present'),dict(op='present')],[dict(op='clear'),dict(op='present')],[dict(op='schedule'),setv('irq_flag',0),dict(op='schedule'),setv('psx_cycle_count',1499),dict(op='service'),setv('psx_cycle_count',1500),dict(op='service'),setv('psx_cycle_count',2000),dict(op='service')],[setv('s_ring_write',3),setv('stat_reg',129),dict(op='arrive',delivered=1,sequence=56),dict(op='present')]):
                            add(setup+tail)
    r=random.Random(seed)
    fields={'irq_flag':[0,1,2,3,4,5,6,7], 'stat_reg':[0,1,32,64,128,255], 's_ring_read':list(range(8)), 's_ring_write':list(range(8)), 'last_sector_lba':[0,1,449999], 's_source_clock':[0,1], 'psx_cycle_count':[0,1,499,500,501,999,1000,1500,2000,2**64-2001], 's_source_ready_due':[0,1,499,500,501,999,1000,1500,2000,2**64-2001]}
    for _ in range(256):
        ops=[]
        for step in range(96):
            choice=r.randrange(12)
            if choice<4:
                f=r.choice(list(fields));ops.append(setv(f,r.choice(fields[f])))
            elif choice<7:ops.append(dict(op='arrive',delivered=r.randrange(2),sequence=r.choice([0,1,2**64-1,r.getrandbits(64)])))
            elif choice==7:ops.append(dict(op='response',read=r.randrange(16),count=r.randrange(17),bytes=[r.randrange(256) for _ in range(16)]))
            else:ops.append(dict(op=r.choice(['clear','present','schedule','service','reset'])))
        add(ops)
    return dict(schema='t172-cd-pending-experiment-v1',cases=cases)

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('output');p.add_argument('--seed',type=int,default=172092010);a=p.parse_args()
    with Path(a.output).open('x') as f:json.dump(generate(a.seed),f,indent=2)
