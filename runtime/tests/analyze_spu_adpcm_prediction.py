"""Compare explicit documentation/measurement hypotheses with public host PCM.

This is an experiment, not a hardware oracle or inverse PCM decoder.
Integer-phase Gaussian weights come from PSX-SPX a253f078 SPU section.
The final 3/4 host gain is T77's public-output contract, not PSX hardware.
"""
import argparse
from collections import defaultdict
import json
from pathlib import Path

COEFFICIENTS = [(0, 0), (60, 0), (115, -52), (98, -55), (122, -60)] + [(0, 0)] * 11
GAUSSIAN_ZERO = [0x12C7, 0x59B3, 0x1307, -1]


def decoded_samples(case, rounding):
    recent = older = 0
    result = []
    for preload in case["preload"][1:]:
        header, _flags, *data = preload["bytes"]
        shift, filt = header & 15, header >> 4
        for byte in data:
            for nibble in [byte & 15, byte >> 4]:
                signed = nibble if nibble < 8 else nibble - 16
                # Constant-input experiments distinguish reserved shifts from XA.
                residual = signed * 4096 // (1 << shift) if shift <= 12 else (-128 if signed < 0 else 0)
                a, b = recent * COEFFICIENTS[filt][0], older * COEFFICIENTS[filt][1]
                if rounding == "combined_round":
                    prediction = (a + b + 32) // 64
                elif rounding == "combined_floor":
                    prediction = (a + b) // 64
                else:
                    prediction = a // 64 + b // 64
                sample = max(-32768, min(32767, residual + prediction))
                older, recent = recent, sample
                result.append(sample)
    return result


def analyze(matrix, trace):
    audio = defaultdict(dict)
    with trace.open() as f:
        next(f)
        for line in f:
            row = json.loads(line)
            if row["kind"] == "sample":
                audio[row["case_id"]][row["sample_index"]] = row["audio"][0]
    results = []
    for rounding in ["combined_round", "combined_floor", "separate_floor"]:
        for interpolation in ["per_term_floor", "combined_floor"]:
            errors = defaultdict(list)
            for case in matrix["cases"]:
                if not case["id"].startswith("decode_"):
                    continue
                decoded = decoded_samples(case, rounding)
                for tick in range(35, 83):
                    # Alignment was tested with filter-zero warmup controls.
                    index = tick - 3
                    products = [a * b for a, b in zip(decoded[index-3:index+1], GAUSSIAN_ZERO)]
                    value = (sum(products) // 32768 if interpolation == "combined_floor"
                             else sum(p // 32768 for p in products))
                    for gain in [24576, 32766, 32766]:
                        value = value * gain // 32768
                    value = (3 * value + 2) // 4
                    errors[case["id"].split("_")[1]].append(abs(value - audio[case["id"]][tick]))
            results.append({"prediction": rounding, "interpolation": interpolation,
                            "by_filter": {k: {"total_abs_error": sum(v), "max_abs_error": max(v),
                                               "observations": len(v), "exact": v.count(0)}
                                          for k, v in errors.items()}})
    return results


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("matrix", type=Path)
    parser.add_argument("trace", type=Path)
    args = parser.parse_args()
    print(json.dumps(analyze(json.loads(args.matrix.read_text()), args.trace), indent=2))
