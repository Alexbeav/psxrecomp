"""Validate the complete authored seek input sequence and integer results."""
import argparse
from contextlib import ExitStack
import hashlib
import json
from pathlib import Path


def validate(matrix_path, paths):
    matrix = json.loads(matrix_path.read_text())
    expected_hash = hashlib.sha256(matrix_path.read_bytes()).hexdigest()
    digests = [hashlib.sha256() for _ in paths]
    differences, examples = 0, []
    with ExitStack() as stack:
        files = [stack.enter_context(path.open()) for path in paths]
        for file in files:
            if json.loads(next(file))["metadata"]["matrix_sha256"] != expected_hash:
                raise ValueError("seek trace belongs to another matrix")
        for case in matrix["cases"]:
            observed = []
            for file, digest in zip(files, digests):
                row = json.loads(next(file, "null"))
                if not isinstance(row, dict) or set(row) != {"case_id", "cycles"}:
                    raise ValueError("missing or malformed seek result")
                if row["case_id"] != case["id"]:
                    raise ValueError("wrong seek case identity")
                if type(row["cycles"]) is not int or not 0 <= row["cycles"] < 2**31:
                    raise ValueError("invalid cycle delay")
                observed.append(row["cycles"])
                digest.update((json.dumps(row, sort_keys=True, separators=(",", ":")) + "\n").encode())
            if len(observed) == 2 and observed[0] != observed[1]:
                differences += 1
                if len(examples) < 20:
                    examples.append(dict(case=case, first=observed[0], second=observed[1]))
        if any(next(file, None) is not None for file in files):
            raise ValueError("extra seek results")
    return dict(cases=len(matrix["cases"]), different_results=differences,
                normalized_sha256=[d.hexdigest() for d in digests], examples=examples)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("matrix", type=Path)
    parser.add_argument("traces", nargs="+", type=Path)
    args = parser.parse_args()
    print(json.dumps(validate(args.matrix, args.traces), indent=2))
