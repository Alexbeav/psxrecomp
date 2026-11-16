"""Range changes and advances ending near the original scanline after wrap."""
import json


def matrix():
    cases = []
    for before in [470000, 480000, 490000, 500000, 510000]:
        for end in [0, 1, 231, 232, 256, 257, 262, 263, 511]:
            for elapsed in [560000, 565000, 566000, 567000, 568000, 570000]:
                cases.append(dict(id=f'wrap_{before}_{end}_{elapsed}', operations=[
                    dict(op='to', now=600000), dict(op='raster', elapsed=before),
                    dict(op='gp1', word=0x070003ff | (end << 10)), dict(op='raster', elapsed=elapsed)]))
    return dict(schema='t172-service-clock-experiment-v1', cases=cases)


if __name__ == '__main__':
    print(json.dumps(matrix(), indent=2))
