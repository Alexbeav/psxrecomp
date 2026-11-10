#!/usr/bin/env python3
"""Create and finalize automatic Workbench intervention ledgers."""

from __future__ import annotations

import hashlib
import json
import os
import time
import uuid
from datetime import datetime, timezone
from pathlib import Path
from typing import Any


CATEGORIES = {
    "configuration_edit",
    "manual_admission_or_heal",
    "route_creation",
    "build_correction",
    "judgment_decision",
    "external_input_preparation",
}


class LedgerError(ValueError):
    """The intervention ledger is invalid or cannot be updated safely."""


def _utc_now() -> str:
    return datetime.now(timezone.utc).isoformat().replace("+00:00", "Z")


def _canonical(value: Any) -> bytes:
    return json.dumps(value, sort_keys=True, separators=(",", ":")).encode()


def _digest(value: Any) -> str:
    return hashlib.sha256(_canonical(value)).hexdigest()


def _write_atomic(path: Path, value: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp-" + uuid.uuid4().hex)
    temporary.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    os.replace(temporary, path)


def create(run_id: str, command: list[str], scope: str) -> dict[str, Any]:
    if not run_id or not scope or not command or not all(command):
        raise LedgerError("run_id, scope, and command are required")
    started = _utc_now()
    return {
        "schema_version": 1,
        "run_id": run_id,
        "scope": scope,
        "command": list(command),
        "started_at": started,
        "finished_at": None,
        "status": "running",
        "events": [],
        "time": {
            "wall_seconds": None,
            "machine_seconds": None,
            "agent_seconds": None,
        },
        "compute_cost": None,
        "interventions": {category: 0 for category in sorted(CATEGORIES)},
        "intervention_total": 0,
    }


def record_phase(ledger: dict[str, Any], phase: str, status: str,
                 detail: str = "", *, machine_seconds: float | None = None) -> None:
    if not phase or status not in {"start", "pass", "fail", "abstain"}:
        raise LedgerError("automation phase and status are invalid")
    ledger["events"].append({
        "event_id": "event_" + uuid.uuid4().hex,
        "kind": "automation_phase",
        "phase": phase,
        "status": status,
        "detail": detail,
        "recorded_at": _utc_now(),
        "machine_seconds": machine_seconds,
    })


def record_intervention(ledger: dict[str, Any], category: str, detail: str,
                        *, wall_seconds: float | None = None,
                        machine_seconds: float | None = None,
                        agent_seconds: float | None = None,
                        compute_cost: float | None = None) -> None:
    if category not in CATEGORIES:
        raise LedgerError(f"unsupported intervention category: {category}")
    if not detail:
        raise LedgerError("intervention detail is required")
    values = (wall_seconds, machine_seconds, agent_seconds, compute_cost)
    if any(value is not None and value < 0 for value in values):
        raise LedgerError("intervention measurements cannot be negative")
    ledger["events"].append({
        "event_id": "event_" + uuid.uuid4().hex,
        "kind": "human_intervention",
        "category": category,
        "detail": detail,
        "recorded_at": _utc_now(),
        "wall_seconds": wall_seconds,
        "machine_seconds": machine_seconds,
        "agent_seconds": agent_seconds,
        "compute_cost": compute_cost,
    })
    ledger["interventions"][category] += 1
    ledger["intervention_total"] += 1


def finalize(ledger: dict[str, Any], status: str, started_monotonic: float,
             *, machine_seconds: float | None = None,
             agent_seconds: float | None = None,
             compute_cost: float | None = None) -> dict[str, Any]:
    if status not in {"complete", "failed", "abstain"}:
        raise LedgerError("final status is invalid")
    ledger["status"] = status
    ledger["finished_at"] = _utc_now()
    ledger["time"] = {
        "wall_seconds": max(0.0, time.monotonic() - started_monotonic),
        "machine_seconds": machine_seconds,
        "agent_seconds": agent_seconds,
    }
    ledger["compute_cost"] = compute_cost
    body = {key: value for key, value in ledger.items() if key != "ledger_sha256"}
    ledger["ledger_sha256"] = _digest(body)
    return ledger


def write(path: Path, ledger: dict[str, Any]) -> None:
    _write_atomic(path, ledger)


def sync(path: Path, ledger: dict[str, Any]) -> None:
    """Keep externally recorded human events when the automation writes."""
    if path.is_file():
        external = json.loads(path.read_text(encoding="utf-8"))
        known = {event["event_id"] for event in ledger["events"]}
        for event in external.get("events", []):
            if (event.get("kind") == "human_intervention"
                    and event.get("event_id") not in known):
                ledger["events"].append(event)
                known.add(event["event_id"])
        ledger["interventions"] = {
            category: 0 for category in sorted(CATEGORIES)
        }
        for event in ledger["events"]:
            if event.get("kind") == "human_intervention":
                ledger["interventions"][event["category"]] += 1
        ledger["intervention_total"] = sum(ledger["interventions"].values())
    write(path, ledger)
