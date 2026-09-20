"""Post-implementation mixed texture and synthetic VRAM holdouts."""
import json
import random
rng=random.Random(172092005);cases=[]
for case in range(192):
    ops=[dict(op='digest')]
    points=[(rng.randrange(256),rng.randrange(256)) for _ in range(4)]+[(0,0),(255,255)]
    fields=dict(mode=[0,1,2],page=[0,1,16,31,128,256,65535,rng.randrange(65536)],
                clut=[0,63,64,32767,32768,65535,rng.randrange(65536)],
                window=[0,0xffffffff,rng.getrandbits(32)],raw=[0,1],load_clut=[0,1],extra_work=[0,1,1000000000])
    for i in range(128):
        choice=rng.randrange(10)
        if choice<4:
            u,v=rng.choice(points);ops.append(dict(op='fetch',u=u,v=v))
        elif choice<6:
            field=rng.choice(list(fields));ops.append(dict(op='set',field=field,value=rng.choice(fields[field])))
        elif choice==6:ops.append(dict(op='palette'))
        elif choice==7:ops.append(dict(op='control',action=rng.randrange(5),page=rng.choice([0,1,31,0x19f,rng.getrandbits(32)])))
        elif choice==8:ops.append(dict(op='write',index=rng.choice([0,1,3,4,1023,1024,524287,rng.randrange(524288)]),value=rng.randrange(65536)))
        else:ops.append(dict(op='sprite',command=rng.getrandbits(32),words=[rng.getrandbits(32) for _ in range(4)]))
    ops.append(dict(op='digest'));cases.append(dict(id=f'holdout_{case}',seed=rng.getrandbits(32),operations=ops))
print(json.dumps(dict(schema='t172-texture-experiment-v1',cases=cases),indent=2))
