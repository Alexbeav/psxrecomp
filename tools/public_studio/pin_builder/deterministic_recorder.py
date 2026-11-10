#!/usr/bin/env python3
"""Compare normalized recorder streams without conflating evidence domains."""

from __future__ import annotations

import hashlib
import json
from pathlib import Path
from typing import Any


PREFIX = "PSXWB_ORACLE\t"


class DeterminismError(ValueError):
    """Recorder evidence is invalid or not deterministic."""


def _canonical(value: Any) -> bytes:
    return json.dumps(value, sort_keys=True, separators=(",", ":")).encode()


def _hash(value: Any) -> str:
    return hashlib.sha256(_canonical(value)).hexdigest()


def load_records(path: Path) -> list[dict[str, Any]]:
    records = []
    for line_number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        if not line.strip():
            continue
        if line.startswith(PREFIX):
            line = line[len(PREFIX):]
        value = json.loads(line)
        if not isinstance(value, dict):
            raise DeterminismError(f"record {line_number} is not an object")
        records.append(value)
    if not records or records[0].get("record") != "start" or records[-1].get("record") != "complete":
        raise DeterminismError("normalized stream is incomplete")
    return records


def streams(records: list[dict[str, Any]]) -> dict[str, list[dict[str, Any]]]:
    state = []
    trace = []
    render = []
    for record in records:
        if record.get("record") != "checkpoint":
            continue
        frame = record.get("relative_frame")
        if not isinstance(frame, int) or frame < 0:
            raise DeterminismError("checkpoint frame is invalid")
        if "state" not in record:
            raise DeterminismError(f"frame {frame} has no state")
        state.append({"frame": frame, "state": record["state"]})
        trace.append({"frame": frame, "trace": record.get("trace", {
            "executed_addresses": [],
            "function_candidates": [],
            "dispatch_candidates": [],
            "indirect_targets": [],
        })})
        if record.get("render") is not None:
            render.append({
                "frame": frame,
                "sha256": record["render"].get("sha256"),
                "width": record["render"].get("width"),
                "height": record["render"].get("height"),
                "bpp": record["render"].get("bpp"),
            })
    return {"state": state, "trace": trace, "render": render}


def qualify(first: Path, second: Path, *, backend_id: str = "pcsx-redux") -> dict[str, Any]:
    left_records = load_records(first)
    right_records = load_records(second)
    for label, records in (("first", left_records), ("second", right_records)):
        ids = {record.get("backend_id") for record in records}
        if ids != {backend_id}:
            raise DeterminismError(f"{label} stream backend identity is invalid")
    left = streams(left_records)
    right = streams(right_records)
    result: dict[str, Any] = {
        "schema_version": 1,
        "backend_id": backend_id,
        "required_runs": 2,
        "domains": {},
    }
    for domain in ("trace", "state", "render"):
        left_hash = _hash(left[domain])
        right_hash = _hash(right[domain])
        result["domains"][domain] = {
            "first_sha256": left_hash,
            "second_sha256": right_hash,
            "identical": left_hash == right_hash,
            "record_count": len(left[domain]),
        }
    result["qualified"] = (
        result["domains"]["trace"]["identical"]
        and result["domains"]["state"]["identical"]
    )
    result["status"] = "qualified" if result["qualified"] else "failed"
    return result

