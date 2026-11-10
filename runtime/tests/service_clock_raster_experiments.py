"""Isolate declared nested-raster updates from service scheduling."""
import json


def matrix():
    cases = []
    def add(label, operations):
        cases.append(dict(id=f'{label}_{len(cases)}', operations=operations))
    for mode in [0, 4, 32, 36, 8]:
        setup = [dict(op='gp1', word=0x08000000 | mode)]
        add('micro', setup + [dict(op='raster', elapsed=1) for _ in range(5000)])
        add('fields', setup + [dict(op='raster', elapsed=2137) for _ in range(1200)])
        for cycles in [0, 2026, 2027, 2152, 2153, 551053, 563969, 565995, 566122, 568274, 1117175, 2000000]:
            add('bulk', setup + [dict(op='raster', elapsed=cycles)])
    for time in [127, 128, 129, 2026, 2027, 2048, 2153, 551053, 566122, 1117175]:
        for op in ['to', 'dma_write', 'frame_end', 'cpu_boundary']:
            add('callback', [dict(op=op, now=time)])
    return dict(schema='t172-service-clock-experiment-v1', cases=cases)


if __name__ == '__main__':
    print(json.dumps(matrix(), indent=2))
