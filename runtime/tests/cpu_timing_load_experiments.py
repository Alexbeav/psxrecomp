"""Load overlap and batch-state probes using authored inputs only."""
import json


def matrix():
    cases = []
    def set_(field, value):
        return dict(op='set', field=field, value=value)
    for rt in [0, 1, 2, 31]:
        for which in [0, 1, 2, 31]:
            for fudge in [0, 1, 2, 31, 32]:
                for value in [0, 1, 2, 5, 7, 255]:
                    for mask in [0, 2, 0xffffffff]:
                        cases.append(dict(id=f'load_{rt}_{which}_{fudge}_{value}_{mask}', operations=[
                            set_('which', which), set_('fudge', fudge),
                            dict(op='absorb', index=which, value=value),
                            dict(op='word', addr=0, rt=rt, mask=mask)]))
    for defer in [0, 1, 2]:
        for batch in [0, 1, 63, 64, 65, 0xffffffff]:
            for limit in [0, 1, 64]:
                for op in ['begin', 'end', 'flush', 'local_begin', 'local_end', 'charge']:
                    call = dict(op=op)
                    if op == 'charge':
                        call['cycles'] = 2
                    cases.append(dict(id=f'batch_{defer}_{batch}_{limit}_{op}', operations=[
                        set_('defer', defer), set_('batch', batch), set_('limit', limit), call]))
    for local in [0, 1, 63, 0xffffffff]:
        for flag in ['none', 'device', 'conservative', 'replay']:
            for op in ['local_begin', 'local_end', 'end', 'flush', 'local_null', 'local_publish', 'charge']:
                call = dict(op=op)
                if op == 'charge':
                    call['cycles'] = 2
                operations = [dict(op='local_begin'), set_('local', local)]
                if flag != 'none':
                    operations += [set_(flag, 1)]
                cases.append(dict(id=f'local_{local}_{flag}_{op}', operations=operations+[call]))
    return dict(schema='t172-cpu-timing-experiment-v1', cases=cases)


if __name__ == '__main__':
    print(json.dumps(matrix(), indent=2))
