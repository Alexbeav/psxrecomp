"""Author opaque SPU snapshot roundtrips; compare all restored observations."""
import argparse
import json
from pathlib import Path
from spu_envelope_experiments import matrix, write, tick


def snapshot_matrix():
    result = matrix()
    selected = []
    prefixes = ("sweep_counter_", "sweep_fixed_interposition_", "adsr_phases_",
                "phase_write_", "attack_log_write_", "adsr_keyoff_")
    for case in result["cases"]:
        if not case["id"].startswith(prefixes):
            continue
        # Save before the final observation run, preserving the partial counter.
        setup = case["operations"][:-1]
        selected.append({"id": "snapshot_" + case["id"],
                         "question": "Does restore reproduce every observed register after mutation?",
                         "operations": setup + [{"op": "save"}, tick(40),
                             write(0x1F801C00, 0x1234), write(0x1F801C0C, 0x5678),
                             write(0x1F801C0A, 0xFFFF), tick(9),
                             {"op": "load"}, tick(40)]})
    result["cases"] = selected
    result["matrix_revision"] = "snapshot-1"
    return result


def check(matrix_data, trace):
    from validate_spu_envelope_trace import observations, validate
    result = validate(matrix_data, observations(trace))
    per_case = {}
    for row in observations(trace):
        per_case.setdefault(row["case_id"], []).append(row)
    for case in matrix_data["cases"]:
        save = next(i for i, op in enumerate(case["operations"]) if op["op"] == "save")
        load = next(i for i, op in enumerate(case["operations"]) if op["op"] == "load")
        rows = per_case[case["id"]]
        first = [(r["sample_index"], r["registers"]) for r in rows if r["event_index"] == save + 1]
        restored = [(r["sample_index"], r["registers"]) for r in rows if r["event_index"] == load + 1]
        assert len(first) == 40 and first == restored, case["id"]
        saved = next(r["registers"] for r in rows if r["kind"] == "save")
        loaded = next(r["registers"] for r in rows if r["kind"] == "load")
        assert saved == loaded, case["id"]
    return {"cases": len(per_case), "observations": result["observations"],
            "observation_sha256": result["observation_sha256"], "roundtrip_equal": True}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--trace", type=Path)
    args = parser.parse_args()
    data = snapshot_matrix()
    print(json.dumps(check(data, args.trace) if args.trace else data, indent=2))
