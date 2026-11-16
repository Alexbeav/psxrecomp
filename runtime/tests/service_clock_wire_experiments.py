"""Opaque actual service persistence with baseline-produced paired snapshots."""
import json
from pathlib import Path
import random


def matrix():
    cases = []
    rng = random.Random(1725002)
    for i in range(120):
        operations = []
        time = 0
        for n in range(24):
            if n % 7 == 0:
                operations.append(dict(op='gp1', word=(rng.choice([0, 5, 7, 8]) << 24) | rng.randrange(1 << 24)))
            else:
                time += rng.choice([0, 1, 128, 2153, 551053, rng.randrange(10000)])
                operations.append(dict(op=rng.choice(['to', 'dma_write', 'frame_end', 'cpu_boundary']), now=time))
        cases.append(dict(id=f'wire_{i}', operations=operations))
    return dict(schema='t172-service-clock-wire-experiment-v1', cases=cases,
                policy='Use actual public384-byte service and160-byte paired raster records; never construct guessed fields')


def replay(trace):
    with trace.open() as file:
        next(file)
        samples = [json.loads(line) for n, line in enumerate(file) if n % 13 == 0]
    cases = []
    for i, row in enumerate(samples):
        operations = [dict(op='restore_wire', service_wire_hex=row['service_wire_hex'],
                           full_raster_wire_hex=row['full_raster_wire_hex'])]
        time = row['state'][0]
        for op, delta in [('to', 0), ('dma_write', 1), ('to', 127), ('cpu_boundary', 2153), ('frame_end', 551053), ('to', 7)]:
            time += delta
            operations.append(dict(op=op, now=time))
        cases.append(dict(id=f'replay_{i}', operations=operations))
    return dict(schema='t172-service-clock-wire-experiment-v1', cases=cases, source_trace=str(trace))


if __name__ == '__main__':
    import sys
    print(json.dumps(replay(Path(sys.argv[1])) if len(sys.argv) > 1 else matrix(), indent=2))
