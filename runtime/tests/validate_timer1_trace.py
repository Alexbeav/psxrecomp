"""Validate authored Timer1 operation identities and compare public results."""
import argparse
from collections import Counter
from contextlib import ExitStack
import hashlib
import json
from pathlib import Path
from validate_timer2_trace import identities


def validate(matrix_path, paths):
    matrix = json.loads(matrix_path.read_text())
    expected_hash = hashlib.sha256(matrix_path.read_bytes()).hexdigest()
    counts, examples = Counter(), []
    digests = [hashlib.sha256() for _ in paths]
    rows = 0
    fields = ["counter", "target", "read", "accepted"]
    keys = {"case_id", "event_index", "kind", "cycle", *fields}
    with ExitStack() as stack:
        files = [stack.enter_context(path.open()) for path in paths]
        for file in files:
            if json.loads(next(file))["metadata"]["matrix_sha256"] != expected_hash:
                raise ValueError("trace belongs to another input matrix")
        for identity in identities(matrix):
            observed = []
            for file, digest in zip(files, digests):
                row = json.loads(next(file, "null"))
                if not isinstance(row, dict) or set(row) != keys:
                    raise ValueError(f"missing or malformed row: {identity}")
                if tuple(row[k] for k in ["case_id", "event_index", "kind", "cycle"]) != identity:
                    raise ValueError(f"wrong row identity: {identity}")
                for key in fields:
                    lower = -1 if key in ["read", "accepted"] else 0
                    if type(row[key]) is not int or not lower <= row[key] < 2**32:
                        raise ValueError(f"invalid field: {identity}")
                if (row["read"] == -1) != (row["kind"] != "read"):
                    raise ValueError("unexpected read result presence")
                if row["accepted"] not in ([0, 1] if row["kind"] == "write" else [-1]):
                    raise ValueError("unexpected acceptance result")
                digest.update((json.dumps(row, sort_keys=True, separators=(",", ":")) + "\n").encode())
                observed.append(row)
            rows += 1
            if len(observed) == 2:
                for key in fields:
                    if observed[0][key] != observed[1][key]:
                        counts[key] += 1
                        if len(examples) < 20:
                            examples.append(dict(identity=identity, field=key,
                                                 first=observed[0][key], second=observed[1][key]))
        if any(next(file, None) is not None for file in files):
            raise ValueError("extra rows")
    return dict(observations=rows, normalized_sha256=[d.hexdigest() for d in digests],
                different_fields=dict(counts), examples=examples)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("matrix", type=Path)
    parser.add_argument("traces", nargs="+", type=Path)
    args = parser.parse_args()
    print(json.dumps(validate(args.matrix, args.traces), indent=2))
