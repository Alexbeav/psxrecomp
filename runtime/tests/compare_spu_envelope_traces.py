"""Classify every observed register against reference and unchanged baseline.

Run validate_spu_envelope_trace.py first for completeness against input operations.
This comparison never treats baseline disagreements as candidate passes.
"""
import argparse
from collections import Counter
from contextlib import ExitStack
import hashlib
import json
from pathlib import Path


def compare(paths):
    counts = Counter()
    per_register = {}
    examples = {}
    cases = {}
    rows = 0
    with ExitStack() as stack:
        files = [stack.enter_context(Path(p).open(encoding="utf-8")) for p in paths]
        metadata = [json.loads(next(f))["metadata"] for f in files]
        assert len({m["matrix_sha256"] for m in metadata}) == 1
        for lines in zip(*files, strict=True):
            r, b, c = [json.loads(line) for line in lines]
            for key in ["case_id", "event_index", "sample_index", "kind"]:
                assert r[key] == b[key] == c[key]
            assert r["registers"].keys() == b["registers"].keys() == c["registers"].keys()
            rows += 1
            for reg, ref in r["registers"].items():
                before, after = b["registers"][reg], c["registers"][reg]
                if after == ref:
                    category = "all_match" if before == ref else "corrected"
                elif before == ref:
                    category = "candidate_only_mismatch"
                elif before == after:
                    category = "unchanged_mismatch"
                else:
                    category = "changed_mismatch"
                counts[category] += 1
                per_register.setdefault(reg, Counter())[category] += 1
                cases.setdefault(category, set()).add(r["case_id"])
                sample = {key: r[key] for key in ["case_id", "event_index", "sample_index", "kind"]}
                sample.update(register=reg, reference=ref, baseline=before, candidate=after)
                bucket = examples.setdefault(category, [])
                if category != "all_match" and len(bucket) < 20:
                    bucket.append(sample)
    return {"observations": rows, "metadata": metadata, "counts": dict(counts),
            "per_register": per_register, "case_counts": {k: len(v) for k, v in cases.items()},
            "cases": {k: sorted(v) for k, v in cases.items() if k != "all_match"},
            "examples": examples,
            "files": {str(p): hashlib.sha256(Path(p).read_bytes()).hexdigest() for p in paths}}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("reference")
    parser.add_argument("baseline")
    parser.add_argument("candidate")
    parser.add_argument("output")
    args = parser.parse_args()
    result = compare([args.reference, args.baseline, args.candidate])
    with Path(args.output).open("x", encoding="utf-8") as f:
        json.dump(result, f, indent=2)
        f.write("\n")
    print(json.dumps({k: result[k] for k in ["observations", "counts", "per_register", "case_counts"]}))
