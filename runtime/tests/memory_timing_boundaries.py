"""Address and width observations, without reading the inherited implementation."""
import json


def matrix():
    addresses = set(range(0x1f800000, 0x1f804001, 64))
    for edge in [0x800000, 0x1f800000, 0x1f800400, 0x1f801000,
                 0x1f801080, 0x1f801100, 0x1f801130, 0x1f801800,
                 0x1f801804, 0x1f801810, 0x1f801820, 0x1f801c00,
                 0x1f801e00, 0x1f802000, 0x1f803000, 0x1f804000,
                 0x1fa00000, 0x1fc00000, 0x1fc80000]:
        addresses.update(edge + delta for delta in [-8, -4, 0, 4, 8])
    cases = []
    for address in sorted(addresses):
        for kind in ['word_slow', 'half_slow', 'byte', 'timing_only', 'lwc2']:
            read = dict(op=kind, addr=address)
            if kind != 'lwc2':
                read.update(rt=3, mask=0)
            cases.append(dict(id=f'boundary_{address:08x}_{kind}', operations=[
                dict(op='set', field='deadline', value=1),
                dict(op='set', field='clock_value', value=1), read]))
    return dict(schema='t172-cpu-timing-memory-experiment-v1', cases=cases)


if __name__ == '__main__':
    print(json.dumps(matrix(), indent=2))
