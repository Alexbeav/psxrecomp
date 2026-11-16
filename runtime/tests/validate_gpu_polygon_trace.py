"""Check polygon case completeness, ordered span arguments, and work results."""
import argparse
from collections import Counter
from contextlib import ExitStack
import hashlib
import json
from pathlib import Path


def validate(matrix_path, paths):
    matrix = json.loads(matrix_path.read_text())
    expected_hash = hashlib.sha256(matrix_path.read_bytes()).hexdigest()
    digests = [hashlib.sha256() for _ in paths]
    counts, examples = Counter(), []
    span_counts = [0 for _ in paths]
    with ExitStack() as stack:
        files = [stack.enter_context(path.open()) for path in paths]
        for file in files:
            if json.loads(next(file))["metadata"]["matrix_sha256"] != expected_hash:
                raise ValueError("polygon trace belongs to another matrix")
        for case in matrix["cases"]:
            observed = []
            for index, (file, digest) in enumerate(zip(files, digests)):
                row = json.loads(next(file, "null"))
                if not isinstance(row, dict) or set(row) != {"case_id", "work", "cost", "spans"}:
                    raise ValueError("missing or malformed polygon result")
                if row["case_id"] != case["id"]:
                    raise ValueError("wrong polygon case identity")
                if not isinstance(row["spans"], list) or any(
                        not isinstance(span, list) or len(span) != 4 for span in row["spans"]):
                    raise ValueError("malformed ordered span list")
                values = [row["work"], row["cost"]] + [value for span in row["spans"] for value in span]
                if any(type(value) is not int or not -(2**31) <= value < 2**31 for value in values):
                    raise ValueError("invalid polygon result integer")
                span_counts[index] += len(row["spans"])
                observed.append(row)
                digest.update((json.dumps(row, sort_keys=True, separators=(",", ":")) + "\n").encode())
            if len(observed) == 2:
                for field in ["work", "cost", "spans"]:
                    if observed[0][field] != observed[1][field]:
                        counts[field] += 1
                        if len(examples) < 20:
                            examples.append(dict(case=case, field=field,
                                                 first=observed[0][field], second=observed[1][field]))
        if any(next(file, None) is not None for file in files):
            raise ValueError("extra polygon results")
    return dict(cases=len(matrix["cases"]), spans=span_counts, different_fields=dict(counts),
                normalized_sha256=[d.hexdigest() for d in digests], examples=examples)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("matrix", type=Path)
    parser.add_argument("traces", nargs="+", type=Path)
    args = parser.parse_args()
    print(json.dumps(validate(args.matrix, args.traces), indent=2))
