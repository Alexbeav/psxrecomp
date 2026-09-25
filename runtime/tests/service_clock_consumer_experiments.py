"""Authored hardware-format packets through the actual runtime callback seam."""
import json
from pathlib import Path


PACKETS = [
    [0x02112233, 0x00040010, 0x00030010],
    [0xe3000000, 0xe407ffff, 0x20123456, 0x00040004, 0x00040010, 0x00100004],
    [0xe6000003, 0x400000ff, 0x00080008, 0x00100010, 0xe6000000],
    [0x01000000, 0x6000ff00, 0x00080008, 0x00030005],
]


def matrix():
    cases = []
    for cost in [0, 1, 127, 128, 129, 1024, 1000000]:
        for packet in PACKETS:
            for route in ['gp0', 'dma_feed', 'mixed']:
                operations = [dict(op='dispatch_cost', value=cost)]
                for i, word in enumerate(packet):
                    op = route if route != 'mixed' else ('gp0' if i % 2 == 0 else 'dma_feed')
                    operations.append(dict(op=op, word=word))
                    if route == 'mixed':
                        operations.append(dict(op='to', now=(i + 1) * 129))
                for time in [1024, 1025, 2048, 4096, 8192, 10000]:
                    operations += [dict(op='to', now=time), dict(op='cpu_boundary', now=time)]
                operations += [dict(op='frame_end', now=10000), dict(op='to', now=12000)]
                for word in PACKETS[0]:
                    operations.append(dict(op='gp0', word=word))
                operations += [dict(op='dma_write', now=12000), dict(op='to', now=15000)]
                cases.append(dict(id=f'consumer_{len(cases)}', operations=operations))
    return dict(schema='t172-service-clock-consumer-experiment-v1', cases=cases,
                policy='Actual callback/GP0 adapter, authored dispatch cost and scripted one-word DMA delivery; no full DMA engine or renderer claim')


def replay(matrix_path, trace):
    original = json.loads(matrix_path.read_text())
    cases = []
    with trace.open() as file:
        next(file)
        ordinal = 0
        for case in original['cases']:
            next(file);cost = 0
            for operation in case['operations']:
                row = json.loads(next(file));ordinal += 1
                if operation['op'] == 'dispatch_cost':
                    cost = operation['value']
                if ordinal % 17 or row['pending_dma_words']:
                    continue
                operations = [dict(op='dispatch_cost', value=cost), dict(op='restore_wire',
                    service_wire_hex=row['service_wire_hex'], full_raster_wire_hex=row['full_raster_wire_hex'])]
                time = row['state'][0]
                operations.append(dict(op='to', now=time + 10000))
                operations += [dict(op='dma_feed', word=w) for w in PACKETS[0]]
                operations += [dict(op='to', now=time + 12000), dict(op='frame_end', now=time + 12000),
                               dict(op='cpu_boundary', now=time + 14000)]
                cases.append(dict(id=f'paired_consumer_{len(cases)}', operations=operations))
    return dict(schema='t172-service-clock-consumer-experiment-v1', cases=cases, source_trace=str(trace))


if __name__ == '__main__':
    import sys
    print(json.dumps(replay(Path(sys.argv[1]), Path(sys.argv[2])) if len(sys.argv) > 1 else matrix(), indent=2))
