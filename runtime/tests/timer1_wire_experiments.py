"""Public saved-state tests across timer1 blank transitions and pending clocks."""
import json
from timer1_experiments import blank, hblank
from timer2_experiments import write, read, advance


def matrix():
    cases = []
    for mode in range(1024):
        if mode & 48:
            continue
        for state in [0, 1]:
            ops = [blank(state), dict(write(8, 3), timer=1), dict(write(4, mode), timer=1),
                   advance(3), hblank(1), dict(read(0), timer=1), blank(1), blank(1),
                   advance(5), blank(0), hblank(7), dict(read(4), timer=1),
                   dict(write(0, 65535), timer=1), advance(9), hblank(2),
                   dict(read(4), timer=1), dict(read(0), timer=1)]
            for cut in [0, 4, 7, 9, 13]:
                cases.append(dict(id=f"mode_{mode:03x}_{state}_cut{cut}",
                                  prefix=ops[:cut], suffix=ops[cut:]))
    for mode in [0, 1, 3, 5, 7, 0x100, 0x101, 0x103, 0x105, 0x107]:
        ops = [advance(3), dict(write(4, mode), timer=1), advance(5), hblank(3),
               blank(1), blank(0), dict(read(0), timer=1), blank(1), blank(0),
               advance(7), hblank(5), dict(read(4), timer=1)]
        for cut in [0, 1, 2, 5]:
            cases.append(dict(id=f"cold_{mode:03x}_cut{cut}", prefix=ops[:cut], suffix=ops[cut:]))
    return dict(schema="t172-timer2-wire-experiment-v1", matrix_revision=1,
                tested_timer=1, cases=cases)


if __name__ == "__main__":
    print(json.dumps(matrix(), indent=2))
