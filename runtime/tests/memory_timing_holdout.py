"""Post-fit mixed-state memory experiments with a fixed reproducible seed."""
import argparse
import json
import random


def matrix(optimized=False):
    rng=random.Random(0x17220920)
    addresses=[0,0x1ffffc,0x200000,0x7ffffc,0x800000,0x1f800000,0x1f8003fc,
               0x1f800400,0x1f801020,0x1f801024,0x1f801040,0x1f80105c,
               0x1f801060,0x1f801070,0x1f801074,0x1f801078,0x1f801080,
               0x1f8010fc,0x1f801100,0x1f80113c,0x1f801140,0x1f801800,
               0x1f80180c,0x1f801810,0x1f801814,0x1f801818,0x1f801820,
               0x1f801824,0x1f801828,0x1f801c00,0x1f801ffc,0x1f802000,
               0x1f802ffc,0x1f803000,0x1fc00000,0x1fc7fffc,0x1fffffff]
    kinds=['word_slow','half_slow','byte','timing_only','lwc2','word','half']
    cases=[]
    for index in range(360):
        ops=[]
        for slot in [0,1,3,7,15,31,32]:
            ops.append(dict(op='absorb',index=slot,value=rng.randrange(256)))
        fields={'cycle':rng.choice([0,127,2**32-16,2**64-2**40]),
                'deadline':rng.choice([0,1,128,2**32,2**64-1]),
                'which':rng.randrange(32),'pending':rng.randrange(33),
                'fudge':rng.randrange(256),'absorb':rng.getrandbits(32),
                'dma_wait':rng.choice([0,1,7,2**32-1,2**32-2,2**32-3,2**32-7]),
                'load_delay':rng.randrange(3),'source_profile':rng.randrange(2),
                'hblank_sample':rng.randrange(2),'clock_value':1,
                'device':rng.randrange(2),'conservative':rng.randrange(2),
                'replay':rng.randrange(2),'lockstep':rng.randrange(2),
                'watch':rng.randrange(2),'recording':rng.randrange(2),
                'batch':rng.choice([0,1,63,2**32-8]),'limit':rng.choice([0,1,64]),
                'defer':rng.randrange(3)}
        if optimized:fields.update(fast_limit=rng.choice([0,1,128,2**64-1]),dma_depth=rng.randrange(2))
        ops += [dict(op='set',field=k,value=v) for k,v in fields.items()]
        if index%3==0:
            ops += [dict(op='local_begin'),dict(op='set',field='local',value=rng.choice([0,17,2**28-1]))]
        for turn in range(8):
            kind=rng.choice(kinds);align=4 if kind in ['word_slow','word','lwc2'] else 2 if kind in ['half_slow','half'] else 1
            address=(rng.choice(addresses)|rng.choice([0,0x80000000,0xa0000000,0xc0000000]))&~(align-1)
            read=dict(op=kind,addr=address)
            if kind!='lwc2':read.update(rt=rng.randrange(32),mask=rng.getrandbits(32))
            ops += [read,dict(op='step',mask=rng.getrandbits(32))]
        ops += [dict(op='local_end'),dict(op='end'),dict(op='flush')]
        cases.append(dict(id=f'holdout_{index}',seed=rng.getrandbits(32),operations=ops))
    return dict(schema='t172-cpu-timing-memory-experiment-v1',cases=cases)


if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--optimized',action='store_true')
    print(json.dumps(matrix(p.parse_args().optimized),indent=2))
