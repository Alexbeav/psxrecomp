"""Synthetic texture experiments authored without reading runtime bodies."""
import argparse
import itertools
import json
import random

def setv(field,value):return dict(op='set',field=field,value=value)
def generate(kind):
    cases=[]
    if kind=='discovery':
        for mode,page,load in itertools.product(range(3),[0,1,16,31,0xffff],[0,1]):
            ops=[dict(op='digest'),setv('mode',mode),setv('page',page),setv('load_clut',load),dict(op='palette')]
            for u,v in [(0,0),(0,0),(1,0),(3,0),(4,0),(15,0),(16,0),(0,1),(0,4),(0,16),(0,64),(255,255),(0,0)]:
                ops.append(dict(op='fetch',u=u,v=v))
            ops.append(dict(op='digest'))
            cases.append(dict(id=f'sample_{mode}_{page}_{load}',seed=197,operations=ops))
        for action in range(5):
            ops=[setv('mode',2),dict(op='write',index=0,value=0x1234),dict(op='fetch',u=0,v=0),
                 dict(op='write',index=0,value=0x5678),dict(op='fetch',u=0,v=0),
                 dict(op='control',action=action,page=0),dict(op='fetch',u=0,v=0),dict(op='digest')]
            cases.append(dict(id=f'coherency_{action}',operations=ops))
        for load,action in itertools.product([0,1],range(5)):
            ops=[setv('clut',64),setv('load_clut',load),dict(op='write',index=0,value=1),dict(op='write',index=1025,value=0x1234),
                 dict(op='palette'),dict(op='fetch',u=0,v=0),dict(op='write',index=1025,value=0x5678),dict(op='palette'),
                 dict(op='fetch',u=0,v=0),dict(op='control',action=action,page=0),dict(op='palette'),dict(op='fetch',u=0,v=0)]
            cases.append(dict(id=f'palette_{load}_{action}',operations=ops))
    elif kind=='geometry':
        rng=random.Random(172092004)
        for command in list(range(256))+[rng.getrandbits(32) for _ in range(1024)]:
            ops=[dict(op='sprite',command=command,words=words) for words in [[0]*4,[0xffffffff]*4,[0,0,0x010103ff,0x02020400],[rng.getrandbits(32) for _ in range(4)]]]
            cases.append(dict(id=f'geometry_{len(cases)}',operations=ops))
    return dict(schema='t172-texture-experiment-v1',cases=cases)

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('kind',choices=['discovery','geometry']);a=p.parse_args();print(json.dumps(generate(a.kind),indent=2))
