"""Refine exact UINT32_MAX publication boundary found by mixed-state holdout."""
import json


def matrix():
    cases = []
    for sink in ['local','batch']:
        for value in [0,1,0xfffffffc,0xfffffffd,0xfffffffe,0xffffffff]:
            for amount in [0,1,2,3,4,0xfffffffe,0xffffffff]:
                for defer in [0,1,2]:
                    ops = [dict(op='set',field='defer',value=defer)]
                    if sink == 'local':
                        ops += [dict(op='local_begin')]
                    ops += [dict(op='set',field=sink,value=value),dict(op='charge',cycles=amount)]
                    cases.append(dict(id=f'overflow_{sink}_{value}_{amount}_{defer}',operations=ops))
    return dict(schema='t172-cpu-timing-experiment-v1',cases=cases)


if __name__ == '__main__':
    print(json.dumps(matrix(),indent=2))
