"""Independent raster-to-Timer1 public caller and paired snapshot fixtures."""
import json
from pathlib import Path
import random
from raster_clock_experiments import advance, gp1


def write(reg, value):
    return dict(op='timer_write', reg=reg, value=value)


def read(reg):
    return dict(op='timer_read', reg=reg)


def matrix():
    cases = []
    for mode in range(1024):
        if mode & 48:
            continue
        for start, end in [(1, 3), (16, 256)]:
            operations = [gp1(0x07000000 | start | (end << 10)), write(8, 3), write(4, mode),
                          write(0, 65534), read(4)]
            for cycles in [0, 1, 2025, 1, 125, 1, 2152, 34441, 551053, 1234567]:
                operations += [advance(cycles, mode % 2), read(0), read(4), read(4)]
            cases.append(dict(id=f'mode_{mode}_{start}', operations=operations))
    rng = random.Random(1724003)
    for i in range(120):
        operations = []
        for _ in range(90):
            kind = rng.randrange(5)
            if kind == 0:
                reg = rng.choice([0, 4, 8]);value = rng.randrange(65536)
                operations.append(write(reg, value & ~48 if reg == 4 else value))
            elif kind == 1:
                operations.append(read(rng.choice([0, 4, 8])))
            elif kind == 2:
                operations.append(gp1((rng.choice([0, 5, 7, 8]) << 24) | rng.randrange(1 << 24)))
            else:
                operations.append(advance(rng.randrange(50000), rng.randrange(2)))
        cases.append(dict(id=f'mixed_{i}', operations=operations))
    return dict(schema='t172-raster-consumer-experiment-v1', cases=cases,
                policy='Actual timer callback and final CPU finish; compare full 80-byte raster and 60-byte timer states')


def replay(trace):
    with trace.open() as file:
        next(file)
        snapshots = [json.loads(line) for n, line in enumerate(file) if n % 47 == 0]
    cases = []
    for i, row in enumerate(snapshots):
        operations = [advance(777), dict(op='restore_consumer', wire_hex=row['wire_hex'], len=80,
                      timer_wire_hex=row['timer_wire_hex'], timer_len=60), read(0), read(4)]
        for cycles in [0, 1, 2027, 2153, 551053, 700003]:
            operations += [advance(cycles, i % 2), read(0), read(4)]
        cases.append(dict(id=f'paired_{i}', operations=operations))
    return dict(schema='t172-raster-consumer-experiment-v1', cases=cases, source_trace=str(trace))


if __name__ == '__main__':
    import sys
    print(json.dumps(replay(Path(sys.argv[1])) if len(sys.argv) > 1 else matrix(), indent=2))
