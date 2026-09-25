"""Check public-wire case completeness and recompute all preservation results."""
import argparse
import hashlib
import json
from pathlib import Path


def validate(matrix_path, trace_path):
    matrix = json.loads(matrix_path.read_text())
    rows = [json.loads(line) for line in trace_path.open()]
    if rows[0]["metadata"]["matrix_sha256"] != hashlib.sha256(matrix_path.read_bytes()).hexdigest():
        raise ValueError("wire trace belongs to another input matrix")
    if len(rows) != len(matrix["cases"]) + 2:
        raise ValueError("missing or extra wire cases")
    failures = dict(roundtrip_failures=0, baseline_replay_failures=0, candidate_replay_failures=0)
    observations = 0
    for case, row in zip(matrix["cases"], rows[1:-1]):
        if row["case_id"] != case["id"]:
            raise ValueError("wrong wire case identity")
        prefix = row["baseline_prefix"]
        if len(prefix) != len(case["prefix"]) + 1 or prefix[-1] != row["snapshot"]:
            raise ValueError("incomplete prefix or wrong snapshot")
        for key in ["baseline_continuation", "baseline_replay", "candidate_replay"]:
            if len(row[key]) != len(case["suffix"]):
                raise ValueError("incomplete suffix")
        for sequence, ops, cycle in [(prefix[1:], case["prefix"], 0)] + [
                (row[key], case["suffix"], row["snapshot"]["cycle"])
                for key in ["baseline_continuation", "baseline_replay", "candidate_replay"]]:
            for result, op in zip(sequence, ops):
                if op["op"] == "advance":
                    cycle += op["cycles"]
                if result["cycle"] != cycle or (result["read"] == -1) != (op["op"] != "read"):
                    raise ValueError("wrong operation identity")
                if len(bytes.fromhex(result["wire"])) != 60:
                    raise ValueError("wrong public wire size")
                observations += 1
        roundtrip = all(row[key]["wire"] == row["snapshot"]["wire"]
                        for key in ["baseline_restore", "candidate_restore"])
        baseline = row["baseline_continuation"] == row["baseline_replay"]
        candidate = row["baseline_continuation"] == row["candidate_replay"]
        for name, reported, actual in [
                ("roundtrip_failures", "byte_roundtrip_equal", roundtrip),
                ("baseline_replay_failures", "baseline_replay_equal", baseline),
                ("candidate_replay_failures", "candidate_replay_equal", candidate)]:
            if row[reported] != actual:
                raise ValueError("incorrect preservation assertion")
            failures[name] += not actual
    return dict(cases=len(matrix["cases"]), observations=observations, **failures)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("matrix", type=Path)
    parser.add_argument("trace", type=Path)
    args = parser.parse_args()
    print(json.dumps(validate(args.matrix, args.trace), indent=2))
