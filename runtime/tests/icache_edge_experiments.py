"""Independent cache edge probes; no inherited implementation inputs."""
import itertools
import json

cases=[]
def add(name,ops):
    cases.append(dict(id=name,operations=ops))
def setv(field,value):
    return dict(op='set',field=field,value=value)

for sequence in itertools.product(['shadow_record','shadow_replay','shadow_end','shadow_abort','cache_reset'],repeat=3):
    ops=[]
    for i,op in enumerate(sequence):
        ops += [dict(op='tag',index=19,value=100+i),dict(op=op)]
    ops += [dict(op='shadow_replay'),dict(op='shadow_end'),dict(op='shadow_record'),dict(op='shadow_abort')]
    add('lifecycle_'+'_'.join(sequence),ops)
for entry,device,conservative,replay,deadline in itertools.product(
        ['boundary','boundary_fn','fetch','fetch_miss','fetch_after_boundary'],[0,1],[0,1],[0,1],[1,1000000]):
    ops=[dict(op='cache_reset'),setv('boundary_callback',1),setv('histogram_active',1),setv('histogram_callback',1),
         setv('device',device),setv('conservative',conservative),setv('replay',replay),setv('deadline',deadline),
         setv('batch',17),setv('limit',37),dict(op='local_begin'),setv('local',23),
         setv('which',7),dict(op='absorb',index=7,value=91),dict(op='absorb',index=0,value=77),
         dict(op=entry,addr=0x8000000c),dict(op=entry,addr=0xa0000000)]
    add(f'publish_{entry}_{device}_{conservative}_{replay}_{deadline}',ops)
for addr in [0,4,8,12,0xffc,0x1000,0x1fffffc,0x7ffffffc,0x80000000,0x9ffffffc,0xa0000000,0xbffffffc,0xfffffffc]:
    for entry in ['fetch','fetch_miss','fetch_after_boundary']:
        add(f'address_{entry}_{addr:08x}',[dict(op='cache_reset'),dict(op=entry,addr=addr),dict(op=entry,addr=addr)])
for bit in range(32):
    for control in sorted({0x804^(1<<bit),0x804|(1<<bit)}):
        add(f'control_{bit}_{control}',[dict(op='tag_fill',value=0xffffffff),dict(op='isolated_store',addr=0x800005a7,control=control)])
print(json.dumps(dict(schema='t172-icache-experiment-v1',cases=cases),indent=2))
