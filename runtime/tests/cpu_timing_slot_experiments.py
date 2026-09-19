"""Resolve slot selection and overlapping load destinations from observations."""
import json


def matrix():
    cases = []
    def setting(field, value):
        return dict(op='set', field=field, value=value)
    for which in [0, 1, 2, 7, 16, 24, 31]:
        for pending in [0, 1, 2, 7, 16, 24, 31, 32]:
            for absorb in [0, 1, 5, 255, 256, 257, 0xffffffff]:
                ops = [setting('which', which), setting('pending', pending), setting('absorb', absorb),
                       dict(op='lds')]
                cases.append(dict(id=f'select_{which}_{pending}_{absorb}', operations=ops))
    for which in [0, 1, 2, 7]:
        for pending in [0, 1, 2, 7, 32]:
            for rt in [0, 1, 2, 7]:
                for mask in [0, 0xffffffff]:
                    ops = [dict(op='absorb', index=i, value=10+i) for i in range(33)]
                    ops += [setting('which', which), setting('pending', pending),
                            setting('absorb', 5), dict(op='word', addr=0, rt=rt, mask=mask)]
                    cases.append(dict(id=f'overwrite_{which}_{pending}_{rt}_{mask}', operations=ops))
    return dict(schema='t172-cpu-timing-experiment-v1', cases=cases)


if __name__ == '__main__':
    print(json.dumps(matrix(), indent=2))
