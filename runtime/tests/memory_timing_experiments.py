"""Independently authored direct memory timing consumer measurements."""
import json


def matrix():
    addresses=[0,0x1ffffc,0x200000,0x7ffffc,0x800000,0x1efffffc,0x1f000000,
               0x1f7ffffc,0x1f800000,0x1f8003fc,0x1f800400,0x1f800ffc,
               0x1f801000,0x1f801070,0x1f8010f0,0x1f801100,0x1f801104,
               0x1f801110,0x1f801120,0x1f801800,0x1f801810,0x1f801c00,
               0x1f802000,0x1fbffffc,0x1fc00000,0x1fc7fffc,0x1fc80000,
               0x80000000,0xa0000000,0xbf801100,0xbfc00000,0xc0000000,0xfffffffc]
    cases=[]
    for source in [0,1]:
        for penalty in [0,1,7]:
            for enabled in [0,1,2]:
                for address in addresses:
                    for kind in ['word_slow','half_slow','byte','timing_only','lwc2']:
                        read=dict(op=kind,addr=address)
                        if kind!='lwc2':read.update(rt=3,mask=0x80000008)
                        ops=[dict(op='set',field=k,value=v) for k,v in
                             [('source_profile',source),('dma_wait',penalty),('load_delay',enabled),
                              ('clock_value',1),('deadline',1)]]
                        ops += [read,dict(op='step',mask=0),read,dict(op='step',mask=8),dict(op='flush')]
                        cases.append(dict(id=f'direct_{source}_{penalty}_{enabled}_{address:08x}_{kind}',
                                          seed=0x51a172,operations=ops))
    return dict(schema='t172-cpu-timing-memory-experiment-v1',cases=cases)


if __name__=='__main__':
    print(json.dumps(matrix(),indent=2))
