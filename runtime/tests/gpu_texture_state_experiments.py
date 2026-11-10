"""Cache collision, control, palette and address experiments on synthetic VRAM."""
import argparse
import itertools
import json

def s(field,value):return dict(op='set',field=field,value=value)
def matrix(kind):
    cases=[]
    if kind=='collisions':
        for mode in range(3):
            for v in [0,1,2,3,4,7,8,15,16,31,32,63,64,127,128,255]:
                ops=[s('mode',mode)]
                for u in range(256):
                    ops += [dict(op='control',action=2,page=0),dict(op='fetch',u=0,v=0),dict(op='fetch',u=u,v=v),dict(op='fetch',u=0,v=0)]
                cases.append(dict(id=f'collision_{mode}_{v}',seed=197,operations=ops))
    elif kind=='controls':
        for action,page1,page2 in itertools.product(range(5),[0,1,16,128,256,511,65535],[0,1,16,128,256,511,0xffffffff]):
            ops=[s('mode',2),s('page',page1),dict(op='fetch',u=0,v=0),dict(op='control',action=action,page=page2),dict(op='fetch',u=0,v=0),dict(op='palette')]
            cases.append(dict(id=f'control_{action}_{page1}_{page2}',seed=197,operations=ops))
    elif kind=='palette':
        for mode,load,clut,nextmode,nextclut in itertools.product(range(3),[0,1],[0,1,63,64,65535],range(3),[0,1,63,64,65535]):
            ops=[s('mode',mode),s('load_clut',load),s('clut',clut),dict(op='palette'),s('mode',nextmode),s('clut',nextclut),dict(op='palette'),dict(op='fetch',u=0,v=0),dict(op='palette'),dict(op='fetch',u=1,v=0)]
            cases.append(dict(id=f'palette_{mode}_{load}_{clut}_{nextmode}_{nextclut}',seed=197,operations=ops))
    elif kind=='address':
        for mode,page,window in itertools.product(range(3),[0,1,15,16,31,32,127,128,255,256,511,2048,65535],[0,1,31,32,1023,1024,32768,0xfffff,0xffffffff]):
            ops=[dict(op='digest'),s('mode',mode),s('page',page),s('window',window),s('clut',65535),s('load_clut',1),dict(op='palette')]
            for u,v in [(0,0),(1,1),(7,7),(8,8),(31,31),(63,63),(127,127),(255,255)]:ops.append(dict(op='fetch',u=u,v=v))
            ops.append(dict(op='digest'))
            cases.append(dict(id=f'address_{mode}_{page}_{window}',seed=197,operations=ops))
    return dict(schema='t172-texture-experiment-v1',cases=cases)

p=argparse.ArgumentParser();p.add_argument('kind',choices=['collisions','controls','palette','address']);a=p.parse_args();print(json.dumps(matrix(a.kind),indent=2))
