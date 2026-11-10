"""Independent expansion of reviewer-supplied local publication counterexamples."""
import json


def matrix():
    threshold=1<<28
    totals=[0,1,threshold-65,threshold-64,threshold-2,threshold-1,threshold,
            threshold+1,threshold+64,2*threshold,0xfffffffe,0xffffffff]
    charges=[0,1,2,63,64,65,threshold-1,threshold,threshold+1,0xffffffff]
    cases=[]
    for total in totals:
        for charge in charges:
            for defer in [0,1,2]:
                for batch in [0,63,0xffffffff]:
                    ops=[dict(op='local_begin'),dict(op='set',field='local',value=total),
                         dict(op='set',field='defer',value=defer),
                         dict(op='set',field='batch',value=batch),
                         dict(op='charge',cycles=charge),dict(op='charge',cycles=1),
                         dict(op='local_end'),dict(op='end')]
                    cases.append(dict(id=f'publication_{total}_{charge}_{defer}_{batch}',operations=ops))
    return dict(schema='t172-cpu-timing-experiment-v1',cases=cases)


if __name__ == '__main__':
    print(json.dumps(matrix(),indent=2))
