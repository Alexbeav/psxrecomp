"""Check every authored timer operation and compare all observable fields."""
import argparse
from collections import Counter
from contextlib import ExitStack
import hashlib
import json
from pathlib import Path


def identities(matrix):
    for case in matrix["cases"]:
        cycle = 0
        yield case["id"], -1, "reset", cycle
        for index, op in enumerate(case["operations"]):
            if op["op"] == "advance":
                cycle += op["cycles"]
            yield case["id"], index, op["op"], cycle


def validate(matrix_path, paths):
    matrix = json.loads(matrix_path.read_text())
    expected_hash = hashlib.sha256(matrix_path.read_bytes()).hexdigest()
    counts, examples = Counter(), []
    digests = [hashlib.sha256() for _ in paths]
    rows = 0
    keys = {"case_id", "event_index", "kind", "cycle", "counter", "target", "pulses", "read", "next"}
    with ExitStack() as stack:
        files = [stack.enter_context(path.open()) for path in paths]
        for file in files:
            metadata = json.loads(next(file))["metadata"]
            if metadata["matrix_sha256"] != expected_hash:
                raise ValueError("trace belongs to another input matrix")
        for identity in identities(matrix):
            observations = []
            for file, digest in zip(files, digests):
                row = json.loads(next(file, "null"))
                if not isinstance(row, dict) or set(row) != keys:
                    raise ValueError(f"missing or malformed row: {identity}")
                if tuple(row[k] for k in ["case_id", "event_index", "kind", "cycle"]) != identity:
                    raise ValueError(f"wrong row identity: {identity}")
                for key in ["counter", "target", "pulses", "read", "next"]:
                    lower = -1 if key in ["read", "next"] else 0
                    if type(row[key]) is not int or not lower <= row[key] < 2**32:
                        raise ValueError(f"invalid {key}: {identity}")
                if (row["read"] == -1) != (row["kind"] != "read"):
                    raise ValueError("read result presence does not match operation")
                if (row["next"] == -1) != (row["kind"] not in ["advance", "write"]):
                    raise ValueError("deadline presence does not match operation")
                digest.update((json.dumps(row, sort_keys=True, separators=(",", ":")) + "\n").encode())
                observations.append(row)
            rows += 1
            if len(observations) > 1:
                for key in ["counter", "target", "pulses", "read", "next"]:
                    if observations[0][key] != observations[1][key]:
                        counts[key] += 1
                        if len(examples) < 20:
                            examples.append({"identity": identity, "field": key,
                                             "first": observations[0][key], "second": observations[1][key]})
        if any(next(file, None) is not None for file in files):
            raise ValueError("unexpected trailing observations")
    return {"observations": rows, "normalized_sha256": [d.hexdigest() for d in digests],
            "different_fields": dict(counts), "examples": examples}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("matrix", type=Path)
    parser.add_argument("first", type=Path)
    parser.add_argument("second", type=Path, nargs="?")
    args = parser.parse_args()
    print(json.dumps(validate(args.matrix, [p for p in [args.first, args.second] if p]), indent=2))
