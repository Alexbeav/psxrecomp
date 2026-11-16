"""Independent raster timing and display-write input sequences."""
import json
import random


def advance(cycles, observed=1):
    return dict(op='advance', cycles=cycles, observed=observed)


def gp1(word):
    return dict(op='gp1', word=word)


def matrix():
    cases = []
    def add(label, operations):
        cases.append(dict(id=f'{label}_{len(cases)}', operations=operations))
    for mode in range(256):
        add('mode', [gp1(0x08000000 | mode)] + [advance(n) for n in [0, 1, 1, 1, 2, 3, 5, 7, 11, 101, 511, 1000, 3000, 600000]])
    for mode in [0, 8, 32, 36, 40, 44]:
        add('micro', [gp1(0x08000000 | mode)] + [advance(1) for _ in range(5000)])
        add('fields', [gp1(0x08000000 | mode)] + [advance(2137) for _ in range(1100)])
    for command in range(256):
        add('command', [advance(54321), gp1((command << 24) | 0xabcdef), advance(0), advance(5000)])
    for start, end in [(0, 0), (0, 1), (1, 2), (16, 256), (20, 300), (260, 263), (263, 264),
                       (312, 314), (500, 511), (1023, 1023), (256, 16)]:
        for mode in [0, 8, 36, 44]:
            add('range', [gp1(0x08000000 | mode), gp1(0x07000000 | start | (end << 10))]
                + [advance(2137) for _ in range(700)])
    rng = random.Random(1724001)
    for _ in range(160):
        operations = []
        for step in range(60):
            if step % 3:
                operations.append(advance(rng.randrange(100000), step % 2))
            else:
                command = rng.choice([0, 5, 6, 7, 8, 9])
                operations.append(gp1((command << 24) | rng.randrange(1 << 24)))
        add('mixed', operations)
    return dict(schema='t172-raster-clock-experiment-v1', cases=cases,
                expected_outputs=None, policy='Compare complete state, ordered events, status, deadline and actual 80-byte wire')


if __name__ == '__main__':
    print(json.dumps(matrix(), indent=2))
