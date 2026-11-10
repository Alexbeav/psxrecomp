"""Discriminate control masks, palette invalidation and retained sampled data."""
import json
cases=[]
def s(field,value):return dict(op='set',field=field,value=value)
for action in range(5):
    for bit in range(32):
        ops=[s('mode',2),dict(op='control',action=3,page=1<<bit),dict(op='fetch',u=0,v=0),
             dict(op='control',action=action,page=0),dict(op='fetch',u=0,v=0),
             dict(op='control',action=3,page=1<<bit),dict(op='fetch',u=0,v=0)]
        cases.append(dict(id=f'control_{action}_{bit}',seed=197,operations=ops))
for action in range(5):
    ops=[s('clut',64),s('load_clut',1),dict(op='write',index=0,value=1),dict(op='write',index=1025,value=0x1234),
         dict(op='palette'),dict(op='fetch',u=0,v=0),s('load_clut',0),dict(op='write',index=1025,value=0x5678),
         dict(op='control',action=action,page=1),dict(op='palette'),dict(op='fetch',u=0,v=0),
         s('load_clut',1),dict(op='palette'),dict(op='fetch',u=0,v=0)]
    cases.append(dict(id=f'palette_reset_{action}',operations=ops))
for clut in [0,63,64,32767,32768,65535]:
    ops=[s('mode',1),s('clut',clut),s('load_clut',1),dict(op='palette')]
    for value in [0,1,15,16,31,255]:
        ops += [dict(op='write',index=0,value=value),dict(op='control',action=2,page=0),dict(op='fetch',u=0,v=0)]
    cases.append(dict(id=f'palette_address_{clut}',seed=197,operations=ops))
print(json.dumps(dict(schema='t172-texture-experiment-v1',cases=cases),indent=2))
