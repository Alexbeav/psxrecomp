"""Cross-field probes authored after the first CPU observation matrix."""
import json
import random


def matrix():
    cases = []
    def add(label, ops):
        cases.append(dict(id=label, operations=ops))
    def setting(field, value):
        return dict(op='set', field=field, value=value)
    for which in [0, 1, 31]:
        for pending in [0, 1, 31, 32]:
            for fudge in [0, 1, 31, 32, 255]:
                for op in ['base', 'lds', 'deps', 'step']:
                    for mask in [0, 1, 2, 0x80000000, 0xffffffff]:
                        if op in ['base', 'lds'] and mask:
                            continue
                        setup = [dict(op='absorb', index=i, value=(i+1)*3)
                                 for i in range(33)]
                        setup += [setting('which', which), setting('pending', pending),
                                  setting('fudge', fudge), setting('absorb', 257)]
                        call = dict(op=op)
                        if op in ['deps', 'step']:
                            call['mask'] = mask
                        add(f'cross_{which}_{pending}_{fudge}_{op}_{mask}', setup+[call])
    for flag, values in [('device', [1]), ('conservative', [1]), ('replay', [1]),
                         ('load_delay', [0, 2]), ('lockstep', [1]), ('recording', [1]),
                         ('watch', [1]), ('dma_wait', [1, 7, 64]), ('lazy_enable', [0])]:
        for value in values:
            for address in [0, 0x200000, 0x800000, 0x1f800000, 0x80000000, 0xa0000000]:
                for op in ['word', 'half']:
                    add(f'flag_{flag}_{value}_{address}_{op}', [setting(flag, value),
                        dict(op=op, addr=address, rt=1, mask=0), dict(op='step', mask=2)])
    for deadline in [0, 1, 63, 64, 65, 1000000]:
        for device in [0, 1]:
            for conservative in [0, 1]:
                for amount in [0, 1, 63, 64, 65]:
                    add(f'clock_{deadline}_{device}_{conservative}_{amount}', [
                        setting('deadline', deadline), setting('device', device),
                        setting('conservative', conservative), dict(op='charge', cycles=amount),
                        dict(op='charge', cycles=1), dict(op='flush')])
    rng = random.Random(0xC017172)
    for index in range(150):
        setup = [dict(op='absorb', index=i, value=rng.randrange(256)) for i in range(33)]
        setup += [setting('which', rng.randrange(32)), setting('pending', rng.randrange(33)),
                  setting('fudge', rng.randrange(33)), setting('absorb', rng.getrandbits(32))]
        for _ in range(30):
            op = rng.choice(['base', 'deps', 'lds', 'step', 'word', 'half'])
            call = dict(op=op)
            if op in ['deps', 'step', 'word', 'half']:
                call['mask'] = rng.getrandbits(32)
            if op in ['word', 'half']:
                call.update(addr=rng.randrange(0x200000)//4*4, rt=rng.randrange(32))
            setup.append(call)
        add(f'sequence_{index}', setup)
    return dict(schema='t172-cpu-timing-experiment-v1', cases=cases)


if __name__ == '__main__':
    print(json.dumps(matrix(), indent=2))
