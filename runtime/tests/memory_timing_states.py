"""Independent pipeline, publication and read-path experiments."""
import argparse
import json


def matrix(optimized=False):
    variants = [('reset', [])]
    for field, values in {'pending': [0, 3, 7, 31], 'fudge': [0, 3, 31, 255],
                          'which': [3, 7, 31], 'absorb': [1, 255, 256, 2**32-1],
                          'batch': [1, 63, 2**32-16], 'defer': [1],
                          'device': [1], 'conservative': [1], 'replay': [1],
                          'lockstep': [1], 'recording': [1], 'watch': [1],
                          'dma_wait': [1, 7, 255, 2**32-1],
                          'source_profile': [1], 'hblank_sample': [1],
                          'load_delay': [0, 2]}.items():
        for value in values:
            variants.append((f'{field}{value}', [dict(op='set', field=field, value=value)]))
    variants += [('local', [dict(op='local_begin'), dict(op='set',field='local',value=17)]),
                 ('hblank', [dict(op='set',field='source_profile',value=1),
                             dict(op='set',field='hblank_sample',value=1)]),
                 ('absorbing', [dict(op='set',field='which',value=7),
                                dict(op='absorb',index=7,value=19),
                                dict(op='set',field='pending',value=3),
                                dict(op='set',field='absorb',value=11)])]
    if optimized:
        for field, values in {'fast_limit':[1,8,9,2**64-1],'dma_depth':[1]}.items():
            for value in values:
                variants.append((f'{field}{value}',[dict(op='set',field=field,value=value)]))
    cases=[]
    for kind in ['word_slow','half_slow','byte','timing_only','lwc2']:
        for address in [0,0x80000000,0xc0000000,0x1f800000,0x1f801000,0x1f801800,0x1fc00000]:
            for name,setup in variants:
                read=dict(op=kind,addr=address)
                if kind!='lwc2':read.update(rt=3,mask=8)
                cases.append(dict(id=f'state_{kind}_{address:08x}_{name}',operations=[
                    dict(op='set',field='clock_value',value=1),
                    dict(op='set',field='deadline',value=1000000),*setup,read,
                    dict(op='flush'),dict(op='local_publish')]))
    return dict(schema='t172-cpu-timing-memory-experiment-v1',cases=cases)


if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--optimized',action='store_true')
    print(json.dumps(matrix(p.parse_args().optimized),indent=2))
