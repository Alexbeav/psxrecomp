"""Independent instruction-cache interface observations; no old source inputs."""
import json


def matrix():
    cases=[dict(id='resolver',operations=[dict(op='cache_enabled'),dict(op='cache_reset'),
                                       dict(op='cache_enabled')]),
           dict(id='reset_tags',operations=[dict(op='tag_fill',value=0xffffffff),dict(op='cache_reset')]),
           dict(id='shadow',operations=[dict(op='tag',index=7,value=0xabcdefff),
                                       dict(op='shadow_replay'),dict(op='shadow_record'),
                                       dict(op='tag',index=7,value=0x12345678),
                                       dict(op='shadow_replay'),dict(op='tag',index=8,value=0xfeed),
                                       dict(op='shadow_end'),dict(op='shadow_abort')])]
    for kind in ['fetch','fetch_miss','fetch_fn','fetch_interp','fetch_after_boundary','boundary','boundary_fn','histogram_sample']:
        for active in [0,1,2]:
            for callbacks in [False,True]:
                ops=[dict(op='set',field='cache_active',value=active)]
                if callbacks:
                    ops += [dict(op='set',field=k,value=1) for k in
                            ['histogram_active','boundary_callback','histogram_callback']]
                ops += [dict(op=kind,addr=a) for a in
                        [0,0,4,12,16,0x80000000,0xa0000000,0xc0000000,0xbfc00000]]
                cases.append(dict(id=f'entry_{kind}_{active}_{int(callbacks)}',operations=ops))
    for replay in [0,1]:
        for callback in [0,1]:
            cases.append(dict(id=f'enabled_{replay}_{callback}',operations=[
                dict(op='set',field='replay',value=replay),
                dict(op='set',field='boundary_callback',value=callback),
                dict(op='boundary_enabled',include_replay=0),
                dict(op='boundary_enabled',include_replay=1)]))
    return dict(schema='t172-icache-experiment-v1',cases=cases)


if __name__=='__main__':print(json.dumps(matrix(),indent=2))
