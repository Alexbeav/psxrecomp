"""Independent public service-clock event sequencing observations."""
import json
import random


def call(op, now):
    return dict(op=op, now=now)


def matrix():
    cases = []
    kinds = ['to', 'dma_write', 'frame_end', 'cpu_boundary']
    def add(label, operations):
        cases.append(dict(id=f'{label}_{len(cases)}', operations=operations))
    for kind in kinds:
        for time in [0, 1, 2, 7, 31, 127, 128, 129, 2026, 2027, 2152, 2153, 34440, 34441, 551052, 551053, 566000, 2000000]:
            add('single', [call(kind, time), call(kind, time), call('cpu_boundary', time)])
    for first in kinds:
        for second in kinds:
            for delta in [0, 1, 7, 128, 2027, 1000000]:
                add('order', [call(first, 17), call(second, 17 + delta), call('cpu_boundary', 17 + delta),
                              call('to', 16), call('to', 18 + delta)])
    for mode in [0, 4, 8, 32, 36, 40, 44, 255]:
        for start, end in [(16, 256), (0, 0), (0, 1), (1, 2), (256, 16), (500, 511)]:
            operations = [dict(op='gp1', word=0x08000000 | mode),
                          dict(op='gp1', word=0x07000000 | start | (end << 10))]
            for i in range(24):
                operations += [call(kinds[i % 4], i * 50001), call('cpu_boundary', i * 50001)]
            add('display', operations)
    rng = random.Random(1725001)
    for _ in range(240):
        time = 0;operations = []
        for i in range(80):
            if i % 17 == 0:
                operations.append(dict(op='gp1', word=(rng.choice([0, 5, 7, 8]) << 24) | rng.randrange(1 << 24)))
            else:
                time += rng.choice([0, 1, 7, 127, 128, 129, rng.randrange(50000)])
                operations.append(call(rng.choice(kinds), time))
        add('mixed', operations)
    return dict(schema='t172-service-clock-experiment-v1', cases=cases,
                expected_outputs=None, policy='Compare every service state field, raster wire, deadline, return and ordered event')


if __name__ == '__main__':
    print(json.dumps(matrix(), indent=2))
