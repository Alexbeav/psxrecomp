"""Independent public-wire prefix/suffix experiments, including cold replay."""
import json
from timer2_experiments import write, advance, read


def matrix():
    cases = []

    def add(name, operations, cuts):
        for cut in cuts:
            cases.append(dict(id=f"{name}_cut{cut}", prefix=operations[:cut], suffix=operations[cut:]))

    for mode in range(1024):
        ops = [write(8, 17), write(4, mode), advance(3), advance(31), read(4),
               advance(257), read(0), write(0, 65535), advance(9), read(4), read(4)]
        add(f"mode_{mode:03x}", ops, [0, 3, 7, 9])
    for mode in [0x10, 0x18, 0x30, 0x38, 0x50, 0x58, 0x70, 0x78, 0x250, 0x258]:
        for cycles in [0, 1, 24, 65536]:
            ops = [write(8, 0), write(4, mode), advance(cycles), read(4),
                   write(8, 1), write(8, 0), advance(1), read(4), advance(8), read(0)]
            add(f"equality_{mode:03x}_{cycles}", ops, [0, 2, 3, 6])
    for initial in [0, 1, 0x100, 0x101, 0x200, 0x201, 0x300, 0x301]:
        for phase in range(8):
            ops = [write(4, initial), advance(phase), read(0), write(4, 0x200),
                   advance(8), read(0), read(4)]
            add(f"phase_{initial:03x}_{phase}", ops, [0, 2, 3])
    add("cold", [advance(3), read(0), write(4, 512), advance(5), read(0)], [0, 1])
    return dict(schema="t172-timer2-wire-experiment-v1", matrix_revision=2, cases=cases)


if __name__ == "__main__":
    print(json.dumps(matrix(), indent=2))
