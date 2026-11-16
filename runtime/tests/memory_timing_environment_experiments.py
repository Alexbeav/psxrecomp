"""Small controlled-environment and optimized-branch observation matrix."""
import argparse
import json


def matrix(optimized=False):
    cases=[]
    for kind in ['word_slow','half_slow','byte','timing_only','lwc2']:
        for addr in [0,0x1f800000,0x1f801000,0x1f801100,0x1fc00000]:
            for source in [0,1]:
                for hblank in [0,1]:
                    for penalty in [0,7]:
                        read=dict(op=kind,addr=addr)
                        if kind!='lwc2':read.update(rt=3,mask=8)
                        for fast_limit,dma_depth in ([(0,0),(1,0),(2**64-1,0),(2**64-1,1)] if optimized else [(0,0)]):
                            ops=[dict(op='set',field=k,value=v) for k,v in
                                 [('source_profile',source),('hblank_sample',hblank),
                                  ('dma_wait',penalty),('load_delay',2),('clock_value',1),
                                  ('deadline',1),('pending',3),('absorb',17)]]
                            if optimized:
                                ops += [dict(op='set',field='fast_limit',value=fast_limit),
                                        dict(op='set',field='dma_depth',value=dma_depth)]
                            ops += [read,dict(op='step',mask=0),read,dict(op='flush')]
                            cases.append(dict(id=f'env_{kind}_{addr:08x}_{source}_{hblank}_{penalty}_{fast_limit}_{dma_depth}',operations=ops))
    return dict(schema='t172-cpu-timing-memory-experiment-v1',cases=cases)


if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('--optimized',action='store_true')
    args=parser.parse_args();print(json.dumps(matrix(args.optimized),indent=2))
