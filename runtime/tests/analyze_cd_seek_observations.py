"""Fit and check a simple timing model against independently authored inputs.

All numeric terms below are inferred from public input/output samples. This
does not establish hardware timing or qualification of the CD command pipeline.
"""
import argparse
from fractions import Fraction
import json
from pathlib import Path


def analyze(matrix_path, trace_path):
    matrix = json.loads(matrix_path.read_text())
    rows = [json.loads(line) for line in trace_path.open()][1:]
    observations = {}
    for case, row in zip(matrix["cases"], rows):
        assert case["id"] == row["case_id"]
        observations[(case["profile"], case["motor_on"], case["paused"], case["mode"],
                      case["origin"], case["target"])] = row["cycles"]

    def value(distance, profile=0, motor=1, paused=0, mode=0):
        return observations[(profile, motor, paused, mode, 0, distance)]

    floor = value(0)
    slope = Fraction(value(750), 750)
    base = lambda d: max(floor, d * slope.numerator // slope.denominator)
    threshold = next(d for d in range(257, 5001) if value(d) != base(d))
    long_delay = value(threshold) - base(threshold)
    motor_delay = value(0, motor=0) - floor
    pause_delay = value(0, paused=1) - floor
    special_distances = [d for d in range(257) if value(d, profile=1) != base(d)]
    short_delay = value(special_distances[0], profile=1) - base(special_distances[0])
    failures = []
    for case, row in zip(matrix["cases"], rows):
        distance = abs(case["target"] - (case["origin"] if case["motor_on"] else 0))
        delay = base(distance) + (0 if case["motor_on"] else motor_delay)
        if distance >= threshold:
            delay += long_delay
        elif case["paused"]:
            delay += pause_delay // (2 if case["mode"] & 128 else 1)
        elif case["profile"] and distance in special_distances:
            delay += short_delay // (2 if case["mode"] & 128 else 1)
        delay = min(delay, 2147483647)
        if delay != row["cycles"]:
            failures.append(dict(case=case, observed=row["cycles"], predicted=delay))
    return dict(cases=len(rows), minimum=floor, slope=str(slope),
                long_distance_threshold=threshold, long_delay=long_delay,
                motor_delay=motor_delay, pause_delay=pause_delay,
                profile_short_distances=special_distances, profile_short_delay=short_delay,
                failed_predictions=len(failures), examples=failures[:20])


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("matrix", type=Path)
    parser.add_argument("trace", type=Path)
    args = parser.parse_args()
    print(json.dumps(analyze(args.matrix, args.trace), indent=2))
