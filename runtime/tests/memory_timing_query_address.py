"""Physical-address predicate contract: aliases, widths, admission and disabled controls."""
import json


def matrix():
    cases=[]
    for physical in [0,0x1f800000,0x1f801100,0x1f801110,0x1f801120,0x1fc00000]:
        for segment in [0,0x80000000,0xa0000000,0xc0000000]:
            address=physical|segment
            for kind in ['word_slow','half_slow','byte','timing_only','lwc2']:
                for expected in sorted({physical,address,physical+1}):
                    for enabled,source,hblank in [(1,1,1),(0,1,1),(1,0,1),(1,1,0)]:
                        read=dict(op=kind,addr=address)
                        if kind!='lwc2':read.update(rt=3,mask=8)
                        ops=[dict(op='set',field=k,value=v) for k,v in
                             [('hblank_expected',expected),('hblank_sample',hblank),
                              ('source_profile',source),('load_delay',enabled),
                              ('clock_value',1),('deadline',1)]]
                        ops += [read,dict(op='step',mask=0),read]
                        cases.append(dict(id=f'query_{address:08x}_{kind}_{expected:08x}_{enabled}_{source}_{hblank}',
                                          seed=0x172a11a5,operations=ops))
    return dict(schema='t172-cpu-timing-memory-experiment-v1',cases=cases)


if __name__=='__main__':print(json.dumps(matrix(),indent=2))
