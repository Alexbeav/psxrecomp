"""Seeded mixed-operation holdouts designed after the initial implementation."""
import argparse
import json
import random

p=argparse.ArgumentParser();p.add_argument('seed',type=int);args=p.parse_args()
rng=random.Random(args.seed)
cases=[]
entries=['fetch','fetch_miss','fetch_fn','fetch_interp','fetch_after_boundary','boundary','boundary_fn','histogram_sample']
fields={'cache_active':[0,1,2],'histogram_active':[0,1],'boundary_callback':[0,1],
        'histogram_callback':[0,1],'replay':[0,1],'device':[0,1],'conservative':[0,1],
        'batch':[0,1,63,0xfffffff0],'limit':[0,1,64,0xffffffff],'defer':[0,1,100],
        'local':[0,23,0xfffffff0],'cycle':[0,2**32-1,2**40],
        'deadline':[0,1,2**32,2**40+1000],'which':[0,7,31],
        'fudge':[0,32,255],'pending':[0,31,32],'absorb':[0,0xffffffff]}
for index in range(128):
    addresses=[rng.getrandbits(32)&~3 for _ in range(4)]
    addresses += [addresses[0]&0x1fffffff,(addresses[0]&0x1fffffff)|0x80000000,
                  (addresses[0]&0x1fffffff)|0xa0000000,0,4,8,12]
    ops=[dict(op='cache_reset')]
    for slot in range(33):ops.append(dict(op='absorb',index=slot,value=rng.randrange(256)))
    for _ in range(40):
        choice=rng.randrange(10)
        if choice<4:ops.append(dict(op=rng.choice(entries),addr=rng.choice(addresses)))
        elif choice<6:
            field=rng.choice(list(fields));ops.append(dict(op='set',field=field,value=rng.choice(fields[field])))
        elif choice==6:
            addr=rng.choice(addresses)
            ops.append(dict(op='tag',index=(addr>>2)&1023,value=rng.choice([addr,addr|1,addr|2,rng.getrandbits(32)])))
        elif choice==7:ops.append(dict(op='isolated_store',addr=rng.getrandbits(32),control=rng.choice([rng.getrandbits(32),0x804,0,0xffffffff])))
        elif choice==8:ops.append(dict(op=rng.choice(['shadow_record','shadow_replay','shadow_end','shadow_abort','cache_reset'])))
        else:ops.append(dict(op=rng.choice(['local_begin','local_end','local_publish','flush'])))
    ops += [dict(op='shadow_end'),dict(op='shadow_abort'),dict(op='local_end'),dict(op='flush')]
    cases.append(dict(id=f'holdout_{args.seed}_{index}',seed=rng.getrandbits(32),operations=ops))
print(json.dumps(dict(schema='t172-icache-experiment-v1',cases=cases),indent=2))
