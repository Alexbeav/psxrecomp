#!/usr/bin/env python3
"""Reconcile exact generated-code fire counts with retirement evidence."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
from pathlib import Path
from typing import Any


SHA256_RE = re.compile(r"[0-9a-f]{64}\Z")


class CoverageError(ValueError):
    """Generated-code coverage evidence is invalid."""


def _identity(value: Any, label: str) -> str:
    text = str(value).lower()
    if SHA256_RE.fullmatch(text) is None:
        raise CoverageError(f"{label} must be a SHA-256 digest")
    return text


def _count(value: Any, label: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or value < 0:
        raise CoverageError(f"{label} must be a nonnegative integer")
    return value


def reconcile(value: dict[str, Any]) -> dict[str, Any]:
    if value.get("schema_version") != 1:
        raise CoverageError("schema_version must be 1")
    route_id = value.get("route_id")
    if not isinstance(route_id, str) or not route_id:
        raise CoverageError("route_id is required")
    content = _identity(value.get("content_sha256"), "content_sha256")
    records = value.get("records")
    if not isinstance(records, list):
        raise CoverageError("records must be an array")
    seen: set[tuple[str, str]] = set()
    normalized = []
    predicted = 0
    for index, record in enumerate(records):
        if not isinstance(record, dict):
            raise CoverageError(f"records[{index}] must be an object")
        kind = record.get("kind")
        identity = record.get("identity")
        if kind not in {"function", "block"} or not isinstance(identity, str) or not identity:
            raise CoverageError(f"records[{index}] identity is invalid")
        key = (kind, identity)
        if key in seen:
            raise CoverageError(f"duplicate generated-code identity: {kind}:{identity}")
        seen.add(key)
        fires = _count(record.get("fire_count"), f"records[{index}].fire_count")
        retired_per_fire = _count(
            record.get("retired_instructions_per_fire"),
            f"records[{index}].retired_instructions_per_fire",
        )
        retired = fires * retired_per_fire
        predicted += retired
        normalized.append({
            "kind": kind,
            "identity": identity,
            "fire_count": fires,
            "retired_instructions_per_fire": retired_per_fire,
            "predicted_retired_instructions": retired,
        })
    retirement = value.get("retirement")
    if not isinstance(retirement, dict):
        raise CoverageError("retirement is required")
    observed = _count(retirement.get("generated_code"), "retirement.generated_code")
    model = retirement.get("measurement_model")
    if model != "exact-static-instruction-count-per-fire-v1":
        raise CoverageError("retirement measurement_model is unsupported")
    def domain(kind: str) -> dict[str, Any]:
        selected = [item for item in normalized if item["kind"] == kind]
        covered = sum(item["fire_count"] > 0 for item in selected)
        return {
            "covered": covered,
            "total": len(selected),
            "coverage": covered / len(selected) if selected else None,
            "fire_count": sum(item["fire_count"] for item in selected),
        }

    reconciled = predicted == observed
    report = {
        "schema_version": 1,
        "status": "qualified" if reconciled else "failed",
        "route_id": route_id,
        "content_sha256": content,
        "measurement_model": model,
        "records": sorted(normalized, key=lambda item: (item["kind"], item["identity"])),
        "covered_identity_count": sum(item["fire_count"] > 0 for item in normalized),
        "total_identity_count": len(normalized),
        "predicted_retired_instructions": predicted,
        "observed_retired_instructions": observed,
        "reconciled": reconciled,
        "functions": domain("function"),
        "blocks": domain("block"),
        "retirement": {
            "predicted": predicted,
            "observed": observed,
            "reconciled": reconciled,
        },
        "overhead": value.get("overhead", {
            "status": "abstain",
            "reason": "no separate matched-pair overhead input",
        }),
    }
    report["coverage_sha256"] = hashlib.sha256(
        json.dumps(report, sort_keys=True, separators=(",", ":")).encode()
    ).hexdigest()
    return report


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    try:
        if args.output.exists():
            raise CoverageError("output already exists")
        value = json.loads(args.input.read_text(encoding="utf-8"))
        report = reconcile(value)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(
            json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        return 0
    except (OSError, json.JSONDecodeError, CoverageError) as error:
        print(json.dumps({"ok": False, "error": str(error)}, sort_keys=True))
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
