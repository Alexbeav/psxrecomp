"""Actual memory timing consumer probes; decoder and scheduler remain seams."""
import json


def matrix():
    cases = []
    addresses = [0,0x1ffffc,0x200000,0x7ffffc,0x800000,0x1f800000,
                 0x1f8003fc,0x1f800400,0x1f801000,0x1f801100,0x1fc00000,
                 0x80000000,0xa0000000,0xbfc00000,0xc0000000,0xfffffffc]
    for source in [0,1]:
        for wait in [0,7]:
            for delay in [0,1,2]:
                for address in addresses:
                    for kind in ['word','half','byte','timing_only','lwc2']:
                        call = dict(op=kind,addr=address)
                        if kind != 'lwc2':
                            call.update(rt=1,mask=0x80000002)
                        ops = [dict(op='set',field=k,value=v) for k,v in
                               [('source_profile',source),('dma_wait',wait),('load_delay',delay),
                                ('hblank_sample',1),('clock_value',1),('cycle',63),
                                ('deadline',64),('watch',1)]]
                        ops += [call,dict(op='step',mask=0),call,dict(op='step',mask=2),
                                dict(op='flush'),dict(op='local_end')]
                        cases.append(dict(id=f'memory_{source}_{wait}_{delay}_{address}_{kind}',
                                          seed=0x172c0de,operations=ops))
    return dict(schema='t172-cpu-timing-memory-experiment-v1',cases=cases)


if __name__ == '__main__':
    print(json.dumps(matrix(),indent=2))
