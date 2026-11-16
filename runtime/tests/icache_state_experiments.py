"""Authored cache boundary, state, invalidation and shadow lifecycle probes."""
import argparse
import itertools
import json


def matrix(kind):
    cases=[]
    if kind=='boundary':
        for entry in ['fetch','fetch_miss','fetch_interp','fetch_after_boundary','boundary','boundary_fn','histogram_sample']:
            for flags in itertools.product([0,1],repeat=4):
                for context in ['none','batch','local','both','conservative','device']:
                    ops=[dict(op='set',field=k,value=v) for k,v in zip(
                        ['histogram_active','histogram_callback','boundary_callback','replay'],flags)]
                    ops += [dict(op='set',field='cache_active',value=1),dict(op='cache_reset')]
                    if context in ['batch','both']:ops += [dict(op='set',field='batch',value=17)]
                    if context in ['local','both']:ops += [dict(op='local_begin'),dict(op='set',field='local',value=23)]
                    if context in ['conservative','device']:ops += [dict(op='set',field=context,value=1)]
                    ops += [dict(op=entry,addr=0x80000004),dict(op=entry,addr=0x80000004)]
                    cases.append(dict(id=f'boundary_{entry}_{flags}_{context}',operations=ops))
    elif kind=='fetch':
        variants=[('reset',[]),('tag0',[dict(op='tag_fill',value=0)]),
                  ('tagff',[dict(op='tag_fill',value=0xffffffff)]),
                  ('absorbing',[dict(op='set',field='which',value=7),dict(op='absorb',index=7,value=19),
                                dict(op='set',field='pending',value=3),dict(op='set',field='absorb',value=11)])]
        for field,values in {'batch':[17,2**32-4],'defer':[1],'device':[1],
                             'conservative':[1],'replay':[1],'cache_active':[0,2],
                             'which':[3,31],'fudge':[0,255],'pending':[0,3,31],
                             'absorb':[0xffffffff]}.items():
            variants += [(f'{field}{v}',[dict(op='set',field=field,value=v)]) for v in values]
        for entry in ['fetch','fetch_miss','fetch_after_boundary']:
            for address in [0,4,8,12,0x80000004,0xa0000000,0xc0000000,0x9fc00008]:
                for name,setup in variants:
                    ops=[dict(op='cache_reset'),dict(op='set',field='deadline',value=1),*setup,
                         dict(op=entry,addr=address),dict(op=entry,addr=address),dict(op='flush')]
                    cases.append(dict(id=f'fetch_{entry}_{address:08x}_{name}',operations=ops))
    elif kind=='isolated':
        controls=sorted({0,4,0x804,0xffffffff,*[1<<i for i in range(32)]})
        addresses=[0,1,3,4,12,15,0xfff,0x1000,0x80000004,0xa0000fff,0xc0001000,0xffffffff]
        for address,control,active in itertools.product(addresses,controls,[0,1,2]):
            cases.append(dict(id=f'store_{address:08x}_{control:08x}_{active}',operations=[
                dict(op='tag_fill',value=0x12345678),dict(op='set',field='cache_active',value=active),
                dict(op='isolated_store',addr=address,control=control)]))
    elif kind=='shadow':
        for sequence in itertools.product(['shadow_record','shadow_replay','shadow_end','shadow_abort'],repeat=4):
            ops=[]
            for index,op in enumerate(sequence):
                ops += [dict(op='tag',index=7,value=0x1000+index),
                        dict(op='set',field='cache_active',value=index%3),
                        dict(op='set',field='fudge',value=index),dict(op='charge',cycles=3),dict(op=op)]
            ops += [dict(op='shadow_end'),dict(op='shadow_abort')]
            cases.append(dict(id='sequence_'+'_'.join(sequence),operations=ops))
    return dict(schema='t172-icache-experiment-v1',cases=cases)


if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('kind',choices=['boundary','fetch','isolated','shadow'])
    print(json.dumps(matrix(p.parse_args().kind),indent=2))
