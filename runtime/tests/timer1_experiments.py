"""Timer1 inputs from hardware documentation and the declared caller boundary."""
import json
import random
from timer2_experiments import write, read, advance


def hblank(count):
    return dict(op="hblank", count=count)


def blank(value):
    return dict(op="blank", value=value)


def matrix():
    cases = []

    def add(name, ops):
        cases.append(dict(id=name, operations=ops))

    add("reset", [read(0), read(4), advance(7), hblank(3), read(0), read(4),
                  blank(0), advance(7), hblank(3), read(0), blank(1), read(4)])
    for mode in range(1024):
        if mode & 48:
            continue
        for initial in [0, 1]:
            ops = [blank(initial), write(8, 3), write(4, mode), read(4)]
            for state in [initial, 0, 0, 1, 1, 0, 1]:
                ops += [advance(1), hblank(1), read(0), blank(state), read(0),
                        advance(2), hblank(2), read(0), read(4), read(4)]
            add(f"mode_{mode:03x}_{initial}", ops)
    for mode in [0, 8, 1, 3, 5, 7, 9, 11, 13, 15, 0x100, 0x108,
                 0x101, 0x103, 0x105, 0x107, 0x109, 0x10B, 0x10D, 0x10F]:
        for target in [0, 1, 3, 65534, 65535]:
            for start in sorted({0, target, (target + 1) & 65535, 65535}):
                ops = [blank(0), write(8, target), write(4, mode), write(0, start)]
                for count in [0, 1, 2, 7, 65535, 65536, 1000000]:
                    ops += [advance(count), hblank(count), read(0), read(4), read(4)]
                add(f"boundary_{mode:03x}_{target:04x}_{start:04x}", ops)
    for mode in [1, 3, 5, 7, 0x101, 0x103, 0x105, 0x107]:
        for state in [0, 1]:
            for value in [0, 1, 2, 3, 65535]:
                ops = [blank(state), write(8, 3), write(4, mode), advance(2), hblank(2),
                       write(0, value), blank(state), read(0), blank(1 - state), read(0),
                       blank(1 - state), read(0), advance(1), hblank(1), read(4)]
                add(f"edge_{mode:03x}_{state}_{value}", ops)
    for value in [0x400, 0x800, 0x1000, 0x2000, 0x4000, 0x8000, 0xFFCF]:
        add(f"mode_bits_{value:04x}", [write(8, 3), write(4, value), read(4),
            advance(4), hblank(4), read(4), write(4, 0), read(4)])
    rng = random.Random(172101)
    for index in range(1000):
        ops = []
        for _ in range(64):
            choice = rng.randrange(5)
            if choice == 0:
                reg = rng.choice([0, 4, 8])
                value = rng.randrange(65536)
                ops.append(write(reg, value & ~48 if reg == 4 else value))
            elif choice == 1:
                ops.append(read(rng.choice([0, 4, 8])))
            elif choice == 2:
                ops.append(blank(rng.randrange(2)))
            else:
                amount = rng.choice([0, 1, 3, 65535, 65536, rng.randrange(1000000)])
                ops.append(advance(amount) if choice == 3 else hblank(amount))
        add(f"mixed_{index:04d}", ops)
    assert len({c["id"] for c in cases}) == len(cases)
    return dict(schema="t172-timer1-experiment-v1", matrix_revision=2, cases=cases,
                policy="IRQ-disabled modes only; explicit CPU/HBlank advances unsplit; repeated blank levels intentional",
                expected_outputs=None)


if __name__ == "__main__":
    print(json.dumps(matrix(), indent=2))
