"""Validate complete black-box SPU traces without importing reference code.

Protocol: one optional metadata row, then observations in matrix order. Each
observation has case_id, event_index, sample_index, kind and registers (u16 map).
event_index is the input operation index; reset uses -1.
No register values are predicted here. An optional second trace must agree
at every observation; different run metadata is permitted.
"""

import argparse
import hashlib
import json
from pathlib import Path


def events(matrix):
    for case in matrix["cases"]:
        sample = 0
        yield case["id"], -1, sample, "reset"
        for event, op in enumerate(case["operations"]):
            if op["op"] == "write16":
                yield case["id"], event, sample, "write16"
            elif op["op"] == "tick":
                for _ in range(op["samples"]):
                    sample += 1
                    yield case["id"], event, sample, "sample"
            else:
                raise ValueError(f"unsupported input operation: {op['op']}")


def observations(path):
    if path.stat().st_size > 128 * 1024 * 1024:
        raise ValueError(f"trace exceeds 128 MiB: {path}")
    with path.open(encoding="utf-8-sig") as source:
        for number, line in enumerate(source, 1):
            row = json.loads(line)
            if number == 1 and "metadata" in row:
                continue
            yield row


def validate(matrix, first, second=None):
    digest = hashlib.sha256()
    summary = {}
    count = 0
    previous = None
    addresses = [f"0x{address:08x}" for address in matrix["observe"]]
    for case_id, event, sample, kind in events(matrix):
        row = next(first, None)
        if row is None:
            raise ValueError(f"missing observation: {case_id}/{event}/{sample}")
        identity = (row.get("case_id"), row.get("event_index"), row.get("sample_index"), row.get("kind"))
        if identity != (case_id, event, sample, kind):
            raise ValueError(f"wrong observation identity: {identity}; expected {(case_id, event, sample, kind)}")
        registers = row.get("registers")
        if (not isinstance(registers, dict) or set(registers) != set(addresses)
                or any(type(value) is not int or not 0 <= value <= 65535 for value in registers.values())):
            raise ValueError(f"invalid register values: {case_id}/{event}")
        values = [registers[address] for address in addresses]
        canonical = {"case_id": case_id, "event_index": event,
                     "sample_index": sample, "kind": kind, "registers": registers}
        if second is not None:
            other = next(second, None)
            if other is None or any(other.get(key) != value for key, value in canonical.items()):
                raise ValueError(f"repeat differs at {case_id}/{event}/{sample}")
        digest.update((json.dumps(canonical, sort_keys=True, separators=(",", ":")) + "\n").encode())
        # Store envelope changes only; the digest still covers ENDX and status.
        envelope = values[:8]
        if event == -1:
            summary[case_id] = []
            previous = None
        if envelope != previous:
            summary[case_id].append({"event": event, "sample": sample, "values": envelope})
            previous = envelope
        count += 1
    if next(first, None) is not None or (second is not None and next(second, None) is not None):
        raise ValueError("unexpected trailing observation")
    return {"observations": count, "observation_sha256": digest.hexdigest(),
            "repeat_equal": second is not None, "cases": summary}


def self_test():
    matrix = {"observe": [0], "cases": [{"id": "control", "operations": [
        {"op": "write16"}, {"op": "tick", "samples": 2}]}]}
    rows = [{"case_id": c, "event_index": e, "sample_index": s, "kind": k,
             "registers": {"0x00000000": e + 1}} for c, e, s, k in events(matrix)]
    assert validate(matrix, iter(rows), iter(rows))["observations"] == 4
    malformed = [rows[:-1], rows + rows[-1:], rows[:1] + rows[2:],
                 [dict(rows[0], registers={"0x00000000": 65536})] + rows[1:],
                 [dict(rows[0], registers={"0x00000000": True})] + rows[1:]]
    for bad in malformed:
        try:
            validate(matrix, iter(bad))
        except ValueError:
            pass
        else:
            raise AssertionError("invalid trace accepted")
    try:
        validate(matrix, iter(rows), iter([dict(rows[0], registers={"0x00000000": 1})] + rows[1:]))
    except ValueError:
        pass
    else:
        raise AssertionError("unequal repeat accepted")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("matrix", type=Path, nargs="?")
    parser.add_argument("trace", type=Path, nargs="?")
    parser.add_argument("repeat", type=Path, nargs="?")
    parser.add_argument("--summary", type=Path)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        self_test()
        print("trace validator positive and negative controls pass")
    else:
        if args.matrix is None or args.trace is None:
            parser.error("matrix and trace are required")
        matrix = json.loads(args.matrix.read_text(encoding="utf-8-sig"))
        result = validate(matrix, observations(args.trace),
                          observations(args.repeat) if args.repeat else None)
        if args.summary:
            with args.summary.open("x", encoding="utf-8") as target:
                json.dump(result, target, indent=2)
        print(json.dumps({key: value for key, value in result.items() if key != "cases"}))
