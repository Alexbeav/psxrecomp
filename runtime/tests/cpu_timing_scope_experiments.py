"""Real cleanup-scope inputs, including distinct nested local accumulators."""
import json


def matrix():
    cases = []
    for flags in range(16):
        for charges in [(0,0,0,0), (1,2,3,4), (63,1,64,65), (65,64,1,63),
                        (999999,1,999999,1)]:
            for mode in ['normal', 'device', 'conservative', 'replay']:
                for deadline in [0, 64, 1000000]:
                    ops = [dict(op='set', field='deadline', value=deadline)]
                    if mode != 'normal':
                        ops += [dict(op='set', field=mode, value=1)]
                    ops += [dict(op='scope', flags=flags, outer_before=charges[0],
                                 inner_before=charges[1], inner_after=charges[2],
                                 outer_after=charges[3]), dict(op='flush'), dict(op='local_end')]
                    cases.append(dict(id=f'scope_{flags}_{charges}_{mode}_{deadline}',
                                      operations=ops))
    return dict(schema='t172-cpu-timing-wire-experiment-v1', cases=cases)


if __name__ == '__main__':
    print(json.dumps(matrix(), indent=2))
