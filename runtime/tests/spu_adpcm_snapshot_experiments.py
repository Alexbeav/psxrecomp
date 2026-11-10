"""Save and restore nonzero decoder histories, queued samples and loop state."""
import argparse
import json
from pathlib import Path
from spu_adpcm_experiments import matrix, VOICE
from spu_envelope_experiments import write, tick
from spu_snapshot_experiments import check


def snapshot_matrix():
    result = matrix()
    cases = []
    for original in result["cases"]:
        name = original["id"]
        if name.startswith("decode_"):
            _, filt, shift, _pattern = name.split("_")
            if int(filt, 16) > 4 or int(shift, 16) not in [0, 8, 12, 13]:
                continue
        elif not name.startswith(("flags_", "address_", "noise_flags_")):
            continue
        for offset in [1, 13, 27]:
            case = dict(original)
            case["id"] = f"snapshot_{name}_{offset}"
            case["operations"] = original["operations"][:-1] + [
                tick(offset), {"op": "save"}, tick(40),
                write(VOICE + 12, 0x1234), write(VOICE + 14, 0x200),
                write(0x1F801D94, 2), tick(9), {"op": "load"}, tick(40)]
            cases.append(case)
    result["cases"] = cases
    result["matrix_revision"] = "adpcm-snapshot-1"
    return result


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--trace", type=Path)
    args = parser.parse_args()
    data = snapshot_matrix()
    if args.trace:
        # The common validator checks PCM format; explicitly check restored PCM too.
        result = check(data, args.trace)
        from validate_spu_envelope_trace import observations
        grouped = {}
        for row in observations(args.trace):
            grouped.setdefault(row["case_id"], []).append(row)
        for case in data["cases"]:
            saved = next(i for i, op in enumerate(case["operations"]) if op["op"] == "save")
            loaded = next(i for i, op in enumerate(case["operations"]) if op["op"] == "load")
            rows = grouped[case["id"]]
            for first, second in [(saved, loaded), (saved + 1, loaded + 1)]:
                a = [r["audio"] for r in rows if r["event_index"] == first]
                b = [r["audio"] for r in rows if r["event_index"] == second]
                assert a == b, case["id"]
        result["pcm_restore_equal"] = True
        print(json.dumps(result))
    else:
        print(json.dumps(data, indent=2))
