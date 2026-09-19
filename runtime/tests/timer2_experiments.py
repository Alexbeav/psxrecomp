"""Independent timer2 input matrix from PSX-SPX a253f078 docs/timers.md.

Explicit mode reads are intentional: they can clear reached flags. No existing
runtime/emulator implementation or source-profile tests supplied expected data.
"""
import json


def write(reg, value):
    return {"op": "write", "reg": reg, "value": value}


def advance(cycles):
    return {"op": "advance", "cycles": cycles}


def read(reg):
    return {"op": "read", "reg": reg}


def matrix():
    cases = [{"id": "reset_readback", "operations": [read(0), read(4), read(4), read(8)]}]

    def add(name, ops):
        cases.append({"id": name, "operations": ops})

    # All mode combinations, with explicit observations around the first target.
    for mode in range(1024):
        ops = [write(8, 3), write(4, mode), read(4)]
        for _ in range(32):
            ops += [advance(1), read(0)]
        ops += [read(4), read(4), read(8)]
        add(f"mode_{mode:03x}", ops)

    modes = [0, 8, 16, 24, 32, 40, 48, 56, 80, 88, 112, 120,
             144, 152, 176, 184, 208, 216, 240, 248]
    for divisor in [0, 512]:
        for low_mode in modes:
            mode = low_mode | divisor
            for target in [0, 1, 2, 7, 65534, 65535]:
                for start in sorted({0, (target - 1) & 65535, target,
                                     (target + 1) & 65535, 65534, 65535}):
                    ops = [write(8, target), write(4, mode), write(0, start), read(4)]
                    for cycles in [0, 1, 1, 2, 7, 8, 9, 31, 65535, 1]:
                        ops += [advance(cycles), read(0), read(4), read(4)]
                    add(f"boundary_{mode:03x}_{target:04x}_{start:04x}", ops)

    # Hold operations constant but partition clock updates differently.
    for mode in [0, 8, 24, 88, 120, 216, 248, 512, 600, 632, 728, 760]:
        for target in [0, 1, 3, 65535]:
            for total, pieces in [(0, [0]), (8, [8]), (8, [1] * 8),
                                  (31, [31]), (31, [7, 8, 16]),
                                  (65536, [65536]), (65536, [32768, 32768]),
                                  (1048576, [1048576])]:
                ops = [write(8, target), write(4, mode)]
                ops += [advance(n) for n in pieces] + [read(0), read(4), read(4)]
                add(f"partition_{mode:03x}_{target:04x}_{total}_{len(pieces)}", ops)

    for elapsed in range(9):
        for reg in [0, 4, 8]:
            for value in [0, 1, 3, 0x200, 0x201, 0x203, 0x207, 0xFFFF]:
                ops = [write(8, 7), write(4, 0x258), advance(elapsed), write(reg, value)]
                for _ in range(18):
                    ops += [advance(1), read(0)]
                ops += [read(4), read(4)]
                add(f"write_phase_{elapsed}_{reg}_{value:04x}", ops)

    for mode in [0x18, 0x58, 0x98, 0xD8, 0x218, 0x258]:
        for reg in [0, 4, 8]:
            for value in [0, 3, mode]:
                ops = [write(8, 3), write(4, mode), advance(24), read(4),
                       write(reg, value), read(4), advance(24), read(4),
                       write(0, 3), read(4), advance(24), read(4)]
                add(f"rearm_{mode:03x}_{reg}_{value:04x}", ops)
    for initial in [0, 1, 3, 5, 7, 0x200, 0x201]:
        for elapsed in [1, 3, 7, 8, 9]:
            ops = [write(4, initial), advance(elapsed), write(4, 0x200)]
            ops += [advance(1) for _ in range(10)] + [read(4)]
            add(f"divider_switch_{initial:03x}_{elapsed}", ops)
    for value in [0x400, 0x800, 0x1000, 0x2000, 0x4000, 0x8000, 0xFFFF]:
        add(f"mode_write_bits_{value:04x}",
            [write(8, 3), write(4, value), read(4), read(4), advance(1), read(4)])

    add("reset_advance", [advance(1), read(4), advance(7), read(0), read(4)])
    for mode in range(8):
        add(f"stopped_equality_{mode}", [write(8, 0), write(4, mode | 0x58),
            read(4), advance(9), read(4), write(0, 0), read(4), write(8, 1),
            write(0, 1), read(4), advance(8), read(4)])
    import random
    rng = random.Random(172)
    for index in range(120):
        ops = []
        for _ in range(32):
            choice = rng.randrange(4)
            if choice == 0:
                ops.append(write(rng.choice([0, 4, 8]),
                                 rng.choice([0, 1, 3, 7, 0x18, 0x58, 0x200, 0x258, 0xFFFF])))
            elif choice == 1:
                ops.append(read(rng.choice([0, 4, 8])))
            else:
                ops.append(advance(rng.choice([0, 1, 2, 7, 8, 9, 1024, 65535, 65536, 1048576])))
        add(f"mixed_{index:03d}", ops)

    assert len({case["id"] for case in cases}) == len(cases)
    assert sum(len(c["operations"]) for c in cases) < 1_000_000
    return {"schema": "t172-timer2-experiment-v1", "matrix_revision": 3,
            "clock_policy": "Explicit advances are not subdivided; read/write flush including zero elapsed",
            "observation_policy": "Counter/target observation has no side effect; mode read only explicitly",
            "expected_outputs": None, "cases": cases}


if __name__ == "__main__":
    print(json.dumps(matrix(), indent=2))
