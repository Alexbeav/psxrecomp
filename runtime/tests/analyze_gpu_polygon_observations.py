"""Test an independently derived rational scanline hypothesis against spans."""
import argparse
from fractions import Fraction
import json
from pathlib import Path


def signed11(value):
    return (value + 1024) % 2048 - 1024


def predict(case):
    x, y = case["x"], case["y"]
    if max(x) - min(x) >= 1024 or max(y) - min(y) >= 512:
        return 0, []
    area = (x[1] - x[0]) * (y[2] - y[0]) - (x[2] - x[0]) * (y[1] - y[0])
    if area == 0:
        return 0, []
    anchor = next(i for i in range(3) if x[i] == min(x) and x[(i + 1) % 3] > x[i])
    left, top, right, bottom = case["clip"]
    work, spans = 0, []
    middle = sorted(y)[1]
    down = sorted({y[anchor], max(y)} | ({middle} if y[anchor] < middle else set()))
    up = sorted({min(y), y[anchor]} | ({middle} if middle < y[anchor] else set()), reverse=True)
    phases = [(1, range(a, b)) for a, b in zip(down, down[1:])]
    phases += [(-1, range(a - 1, b - 1, -1)) for a, b in zip(up, up[1:])]
    for direction, levels in phases:
        for raw_y in levels:
            physical_y = signed11(raw_y)
            if (direction > 0 and physical_y > bottom) or (direction < 0 and physical_y < top):
                break
            if physical_y < top or physical_y > bottom:
                work += 2
                continue
            if case["interlace"] and (raw_y & 1) == case["skip_field"]:
                continue
            intersections = []
            for a, b in [(0, 1), (1, 2), (2, 0)]:
                if min(y[a], y[b]) <= raw_y < max(y[a], y[b]):
                    intersections.append(Fraction(x[a]) + Fraction((raw_y - y[a]) * (x[b] - x[a]), y[b] - y[a]))
            assert len(intersections) == 2
            lo, hi = sorted(intersections)
            raw_x = -(-lo.numerator // lo.denominator)
            end = -(-hi.numerator // hi.denominator)
            physical_x = signed11(raw_x)
            clipped_x = max(left, physical_x)
            width = min(right + 1, physical_x + end - raw_x) - clipped_x
            if width <= 0:
                continue
            interpolation_x = raw_x + clipped_x - physical_x
            spans.append([raw_y, clipped_x, width, interpolation_x])
            work += width * 2 if case["doubled"] else width + ((width + 1) // 2 if case["masked_or_blended"] else 0)
    return work, spans


def analyze(matrix_path, trace_path):
    cases = json.loads(matrix_path.read_text())["cases"]
    rows = [json.loads(line) for line in trace_path.open()][1:]
    differences, examples = {}, []
    for case, row in zip(cases, rows):
        assert case["id"] == row["case_id"]
        work, spans = predict(case)
        for field, prediction in [("work", work), ("spans", spans)]:
            if prediction != row[field]:
                differences[field] = differences.get(field, 0) + 1
                if len(examples) < 12:
                    examples.append(dict(case=case, field=field, observed=row[field], predicted=prediction))
    return dict(cases=len(cases), different_fields=differences, examples=examples)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("matrix", type=Path)
    parser.add_argument("trace", type=Path)
    args = parser.parse_args()
    print(json.dumps(analyze(args.matrix, args.trace), indent=2))
