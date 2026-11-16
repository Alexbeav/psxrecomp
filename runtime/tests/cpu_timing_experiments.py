"""Independent CPU timing inputs; no baseline implementation is imported."""
import json


def matrix():
    cases = []

    def add(name, operations):
        cases.append(dict(id=name, operations=operations))

    def set_field(field, value):
        return dict(op='set', field=field, value=value)

    for slot in [0, 1, 15, 31, 32]:
        for value in [0, 1, 2, 3, 63, 64, 65, 127, 128, 254, 255]:
            for operation in ['base', 'lds', 'deps', 'step']:
                for mask in [0, 1, 2, 32768, 2147483648, 4294967295]:
                    if operation in ['base', 'lds'] and mask:
                        continue
                    call = dict(op=operation)
                    if operation in ['deps', 'step']:
                        call['mask'] = mask
                    add(f'{operation}_{slot}_{value}_{mask}', [
                        dict(op='absorb', index=slot, value=value),
                        set_field('which', min(slot, 31)),
                        set_field('pending', slot), set_field('absorb', value),
                        call, call, call])
    for amount in [0, 1, 2, 63, 64, 65, 127, 128, 129, 65535, 4294967295]:
        charge = dict(op='charge', cycles=amount)
        for name, ops in [
            ('direct', [charge]),
            ('defer', [dict(op='begin'), charge, dict(op='end')]),
            ('flush', [dict(op='begin'), charge, dict(op='flush'), dict(op='end')]),
            ('local', [dict(op='local_begin'), charge, dict(op='local_end')]),
            ('local_defer', [dict(op='local_begin'), dict(op='begin'), charge,
                             dict(op='end'), dict(op='local_end')]),
        ]:
            add(f'charge_{name}_{amount}', ops)
    for address in [0, 0x1ffffc, 0x200000, 0x7ffffc, 0x800000,
                    0x1f800000, 0x1f8003fc, 0x1f800400, 0x1f801000,
                    0x1fc00000, 0x80000000, 0x801ffffc, 0xa0000000,
                    0xbfc00000, 0xfffffffc]:
        for op in ['word', 'half']:
            for rt in [0, 1, 31]:
                for mask in [0, 1 << rt, 4294967295]:
                    add(f'{op}_{address:08x}_{rt}_{mask}', [
                        dict(op=op, addr=address, rt=rt, mask=mask),
                        dict(op='step', mask=mask), dict(op='step', mask=mask)])
    return dict(schema='t172-cpu-timing-experiment-v1', cases=cases)


if __name__ == '__main__':
    print(json.dumps(matrix(), indent=2))
