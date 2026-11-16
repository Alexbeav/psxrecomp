"""Baseline-produced wire restoration, timing boundaries and fresh mixed histories."""
import json
from pathlib import Path
import random
import struct
from raster_clock_experiments import advance, gp1


def matrix(trace):
    with trace.open() as file:
        next(file)
        samples = [json.loads(line) for n, line in enumerate(file) if n % 79 == 0]
    cases = []
    def add(label, operations):
        cases.append(dict(id=f'{label}_{len(cases)}', operations=operations))
    for n, row in enumerate(samples):
        restore = dict(op='restore', wire_hex=row['wire_hex'], len=80)
        delta = min(row['until_rise'], 2000000)
        suffix = [advance(0), advance(max(0, delta - 1)), advance(1), advance(1),
                  gp1(0x08000024), advance(1200000, n % 2), gp1(0x00000000), advance(173)]
        add('restore', [advance(12345), restore] + suffix)
        if n % 10 == 0:
            state = row['state'][:]
            state[0] += 1 << 40
            state[1] += 1 << 40
            wire = struct.pack('<QQ16I', *state).hex()
            add('long_epoch', [dict(op='restore', wire_hex=wire, len=80)] + suffix)
            for length in [0, 79, 81, 0xffffffff]:
                add('bad_length', [advance(5001), dict(op='restore', wire_hex=wire, len=length), advance(7)])
            state[2] ^= 1
            add('bad_fraction', [dict(op='restore', wire_hex=struct.pack('<QQ16I', *state).hex(), len=80), dict(op='reset'), advance(7)])
    rng = random.Random(1724002)
    for _ in range(180):
        operations = []
        for n in range(80):
            if n % 7 == 0:
                operations.append(dict(op='reset'))
            elif n % 3 == 0:
                operations.append(gp1((rng.choice([0, 5, 7, 8, 69, 71, 72, 255]) << 24) | rng.randrange(1 << 24)))
            else:
                operations.append(advance(rng.randrange(250000), rng.randrange(2)))
        add('holdout', operations)
    return dict(schema='t172-raster-clock-experiment-v1', cases=cases,
                source_trace=str(trace), policy='Actual baseline-produced 80-byte snapshots; preserve rejected-reader state and exact suffix observations')


if __name__ == '__main__':
    import sys
    print(json.dumps(matrix(Path(sys.argv[1])), indent=2))
