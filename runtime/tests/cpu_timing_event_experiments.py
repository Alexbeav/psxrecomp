"""Callback-entry CPU state probes derived from reviewer counterexamples."""
import json


def matrix():
    cases=[]
    for which in [0,1,7,31]:
        for pending in [0,3,31,32]:
            for amount in [0,17,0xffffffff]:
                for mask in [0,0xffffffff]:
                    for rt in [0,3,31]:
                        for kind in ['word','half']:
                            for mode in ['normal','watch','conservative','device','dma_wait','replay']:
                                ops=[dict(op='set',field=k,value=v) for k,v in
                                     [('deadline',1),('which',which),('pending',pending),('absorb',amount)]]
                                if mode!='normal':ops+=[dict(op='set',field=mode,value=1)]
                                ops += [dict(op=kind,addr=0,rt=rt,mask=mask),
                                        dict(op='step',mask=mask),dict(op='flush')]
                                cases.append(dict(id=f'event_{which}_{pending}_{amount}_{mask}_{rt}_{kind}_{mode}',operations=ops))
    return dict(schema='t172-cpu-timing-wire-experiment-v1',cases=cases)


if __name__=='__main__':
    print(json.dumps(matrix(),indent=2))
