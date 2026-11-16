"""Post-implementation mixed-state holdout, authored after commit 144897b7d."""
import json
import random


def matrix():
    rng = random.Random(0x144897B7D)
    cases = []
    fields = {
        'which': [0, 1, 15, 31], 'pending': [0, 1, 15, 31, 32],
        'fudge': [0, 1, 31, 32, 255], 'absorb': [0, 1, 5, 255, 256, 0xffffffff],
        'device': [0, 1], 'conservative': [0, 1], 'replay': [0, 1],
        'load_delay': [0, 1, 2], 'lazy_enable': [0, 1], 'watch': [0, 1],
        'lockstep': [0, 1], 'recording': [0, 1], 'dma_wait': [0, 1, 7],
        'batch': [0, 1, 63, 64, 0xfffffffe, 0xffffffff],
        'limit': [0, 1, 63, 64, 0xffffffff], 'defer': [0, 1, 2],
        'local': [0, 1, 63, 0xfffffffe, 0xffffffff],
        'deadline': [0, 1, 63, 64, 65, 1000, 2**64-1],
        'cycle': [0, 1, 63, 64, 65, 1000, 2**64-2**40],
        'fallback': [0, 1, 0x87654321, 0xffffffff],
    }
    kinds = ['set', 'set', 'set', 'absorb', 'base', 'deps', 'lds', 'step',
             'word', 'half', 'charge', 'charge', 'begin', 'end', 'flush',
             'local_begin', 'local_end', 'local_null', 'local_publish']
    for index in range(400):
        ops = []
        for _ in range(100):
            kind = rng.choice(kinds)
            op = dict(op=kind)
            if kind == 'set':
                field = rng.choice(list(fields))
                op.update(field=field, value=rng.choice(fields[field]))
            elif kind == 'absorb':
                op.update(index=rng.randrange(33), value=rng.randrange(256))
            elif kind == 'charge':
                op['cycles'] = rng.choice([0, 1, 2, 63, 64, 65, 0xfffffffe, 0xffffffff])
            elif kind in ['deps', 'step', 'word', 'half']:
                op['mask'] = rng.getrandbits(32)
                if kind in ['word', 'half']:
                    op.update(addr=rng.choice([0, 0x1ffffc, 0x200000, 0x7ffffc, 0x800000,
                                               0x1f800000, 0x1f801000, 0x1fc00000,
                                               0x80000000, 0xa0000000, 0xfffffffc]),
                              rt=rng.randrange(32))
            ops.append(op)
        cases.append(dict(id=f'holdout_{index}', seed=rng.getrandbits(32), operations=ops))
    return dict(schema='t172-cpu-timing-experiment-v1', cases=cases)


if __name__ == '__main__':
    print(json.dumps(matrix(), indent=2))
