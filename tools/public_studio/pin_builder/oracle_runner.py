#!/usr/bin/env python3
"""Run one frozen route through offline PSX oracle backend adapters."""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
from pathlib import Path
from typing import Any


TOOLS = Path(__file__).resolve().parent
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))

import evidence_namespace  # noqa: E402
import experiment_contract  # noqa: E402
import parse_redux_events  # noqa: E402


BACKENDS = ("beetle", "pcsx-redux", "psxrecomp")
ORACLE_PREFIX = "PSXWB_ORACLE\t"
RUNTIME_VARIABLES = ("EXECUTABLE", "CONTRACT", "EVIDENCE", "BACKEND")


class OracleError(ValueError):
    """The oracle contract, probe, or event stream is invalid."""


def write_json(path: Path, value: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def load_contract(path: Path, variable_values: list[str]) -> dict[str, Any]:
    variables = experiment_contract.parse_variables(variable_values)
    variables.update({name: "${" + name + "}" for name in RUNTIME_VARIABLES})
    value = experiment_contract.expand(
        json.loads(path.read_text(encoding="utf-8")), variables)
    if not isinstance(value, dict):
        raise OracleError("oracle contract must be a JSON object")
    return value


def _require_file(path: Path, expected_hash: str, label: str) -> None:
    if not path.is_file():
        raise OracleError(f"missing {label}: {path}")
    actual = experiment_contract.sha256_file(path)
    if actual.lower() != expected_hash.lower():
        raise OracleError(
            f"{label} hash mismatch: expected {expected_hash}, got {actual}")


def validate_contract(contract: dict[str, Any]) -> dict[str, Any]:
    if contract.get("schema_version") != 1:
        raise OracleError("unsupported oracle-route-contract schema_version")
    contract_id = contract.get("contract_id")
    if not isinstance(contract_id, str) or not contract_id:
        raise OracleError("contract_id must be a nonempty string")

    route = contract.get("route")
    if not isinstance(route, dict) or route.get("clock") != "vblank":
        raise OracleError("route clock must be vblank")
    actual_route_hash = experiment_contract.route_sha256(route)
    if actual_route_hash.lower() != str(route.get("sha256", "")).lower():
        raise OracleError(
            f"route hash mismatch: expected {route.get('sha256')}, got {actual_route_hash}")
    duration = sum(step["frames"] for step in route.get("steps", []))
    if duration - 1 != route.get("target_relative_frame"):
        raise OracleError("route duration does not equal target_relative_frame + 1")

    required = contract.get("required_capabilities")
    if not isinstance(required, list) or not required or len(set(required)) != len(required):
        raise OracleError("required_capabilities must be a unique nonempty list")

    for artifact in contract.get("artifacts", []):
        _require_file(Path(artifact["path"]), artifact["sha256"], artifact["id"])

    records = contract.get("backends")
    if not isinstance(records, list):
        raise OracleError("backends must be a list")
    ids = [record.get("id") for record in records if isinstance(record, dict)]
    if sorted(ids) != sorted(BACKENDS):
        raise OracleError("backends must contain beetle, pcsx-redux, and psxrecomp exactly once")

    summaries = []
    for backend in records:
        backend_id = backend["id"]
        capabilities = backend.get("capabilities")
        if not isinstance(capabilities, list) or len(set(capabilities)) != len(capabilities):
            raise OracleError(f"{backend_id} capabilities must be a unique list")
        missing = sorted(set(required) - set(capabilities))
        if missing:
            raise OracleError(
                f"{backend_id} lacks required capabilities: {', '.join(missing)}")
        executable = Path(backend["executable"])
        _require_file(executable, backend["executable_sha256"], f"{backend_id} executable")
        for name in ("probe", "run"):
            process = backend.get(name)
            if not isinstance(process, dict) or not process.get("command"):
                raise OracleError(f"{backend_id}.{name} command is missing")
            if process["command"][0] != "${EXECUTABLE}":
                raise OracleError(
                    f"{backend_id}.{name} must launch the hashed executable directly")
            if not Path(process.get("working_directory", "")).is_dir():
                raise OracleError(f"{backend_id}.{name} working directory is missing")
            timeout = process.get("timeout_seconds")
            if not isinstance(timeout, int) or timeout < 1:
                raise OracleError(f"{backend_id}.{name} timeout is invalid")
        summaries.append({"id": backend_id, "capabilities": sorted(capabilities)})
    return {
        "ok": True,
        "contract_id": contract_id,
        "route_sha256": actual_route_hash,
        "required_capabilities": sorted(required),
        "backends": sorted(summaries, key=lambda item: item["id"]),
    }


def _replace(value: str, values: dict[str, str]) -> str:
    result = value
    for name, replacement in values.items():
        result = result.replace("${" + name + "}", replacement)
    if "${" in result:
        raise OracleError(f"unresolved process variable: {result}")
    return result


def run_process(
    backend: dict[str, Any], process_name: str, contract_path: Path,
    evidence: Path,
) -> subprocess.CompletedProcess[str]:
    process = backend[process_name]
    values = {
        "EXECUTABLE": str(Path(backend["executable"]).resolve()),
        "CONTRACT": str(contract_path.resolve()),
        "EVIDENCE": str(evidence.resolve()),
        "BACKEND": backend["id"],
    }
    command = [_replace(item, values) for item in process["command"]]
    environment = os.environ.copy()
    environment.update({
        "PSXWB_ORACLE_CONTRACT": values["CONTRACT"],
        "PSXWB_ORACLE_EVIDENCE": values["EVIDENCE"],
        "PSXWB_ORACLE_BACKEND": values["BACKEND"],
    })
    environment.update({key: _replace(value, values)
                        for key, value in process.get("environment", {}).items()})
    try:
        result = subprocess.run(
            command, cwd=process["working_directory"], env=environment,
            capture_output=True, text=True, encoding="utf-8", errors="replace",
            timeout=process["timeout_seconds"], check=False, shell=False)
    except subprocess.TimeoutExpired as error:
        raise OracleError(
            f"{backend['id']} {process_name} timed out after "
            f"{process['timeout_seconds']} seconds") from error
    if result.returncode != 0:
        raise OracleError(
            f"{backend['id']} {process_name} exited with {result.returncode}")
    return result


def parse_probe(backend: dict[str, Any], output: str) -> dict[str, Any]:
    lines = [line for line in output.splitlines() if line.strip()]
    if not lines:
        raise OracleError(f"{backend['id']} probe emitted no JSON")
    try:
        value = json.loads(lines[-1])
    except json.JSONDecodeError as error:
        raise OracleError(f"{backend['id']} probe terminal line is not JSON") from error
    if not isinstance(value, dict) or value.get("schema_version") != 1:
        raise OracleError(f"{backend['id']} probe schema is invalid")
    if value.get("backend_id") != backend["id"]:
        raise OracleError(f"{backend['id']} probe identity mismatch")
    confirmed = value.get("capabilities")
    if not isinstance(confirmed, list):
        raise OracleError(f"{backend['id']} probe capabilities are invalid")
    missing = sorted(set(backend["capabilities"]) - set(confirmed))
    if missing:
        raise OracleError(
            f"{backend['id']} probe did not confirm: {', '.join(missing)}")
    return {"backend_id": backend["id"], "capabilities": sorted(confirmed)}


def parse_normalized(lines: list[str], backend_id: str) -> list[dict[str, Any]]:
    records: list[dict[str, Any]] = []
    for number, line in enumerate(lines, 1):
        marker = line.find(ORACLE_PREFIX)
        if marker < 0:
            continue
        try:
            value = json.loads(line[marker + len(ORACLE_PREFIX):].strip())
        except json.JSONDecodeError as error:
            raise OracleError(f"{backend_id} line {number} has invalid oracle JSON") from error
        if not isinstance(value, dict):
            raise OracleError(f"{backend_id} line {number} oracle record is not an object")
        records.append(value)
    if not records:
        raise OracleError(f"{backend_id} emitted no oracle records")
    return records


def normalize_redux(output: str, backend_id: str) -> list[dict[str, Any]]:
    try:
        events = parse_redux_events.parse_events(output.splitlines(keepends=True))
    except ValueError as error:
        raise OracleError(f"{backend_id} event stream is invalid: {error}") from error
    checkpoints = [event for event in events if event.get("record") == "vsync"]
    records: list[dict[str, Any]] = [{
        "schema_version": 1, "record": "start", "backend_id": backend_id}]
    for relative_frame, event in enumerate(checkpoints):
        state = {key: value for key, value in event.items()
                 if key not in {"record", "frame"}}
        records.append({
            "schema_version": 1,
            "record": "checkpoint", "backend_id": backend_id,
            "relative_frame": relative_frame, "state": state,
        })
    records.append({
        "schema_version": 1,
        "record": "complete", "backend_id": backend_id,
        "relative_frame": len(checkpoints) - 1,
    })
    return records


def validate_records(
    records: list[dict[str, Any]], backend_id: str, target_frame: int,
    start_frame: int = 0,
) -> None:
    if records[0].get("record") != "start":
        raise OracleError(f"{backend_id} first oracle record must be start")
    if records[-1].get("record") != "complete":
        raise OracleError(f"{backend_id} last oracle record must be complete")
    for record in records:
        if record.get("schema_version") != 1:
            raise OracleError(f"{backend_id} oracle record schema is invalid")
        if record.get("backend_id") != backend_id:
            raise OracleError(f"{backend_id} oracle record identity mismatch")
    checkpoints = [record for record in records[1:-1]
                   if record.get("record") == "checkpoint"]
    if len(checkpoints) != len(records) - 2:
        raise OracleError(f"{backend_id} has a non-checkpoint middle record")
    frames = [record.get("relative_frame") for record in checkpoints]
    if start_frame < 0 or target_frame < start_frame:
        raise OracleError(f"{backend_id} segment frame bounds are invalid")
    expected = list(range(start_frame, target_frame + 1))
    if frames != expected:
        raise OracleError(
            f"{backend_id} checkpoint frames must be {start_frame} through {target_frame}")
    if records[-1].get("relative_frame") != target_frame:
        raise OracleError(f"{backend_id} complete frame does not match route target")


def probe_all(contract: dict[str, Any], contract_path: Path) -> list[dict[str, Any]]:
    results = []
    for backend in sorted(contract["backends"], key=lambda item: item["id"]):
        completed = run_process(backend, "probe", contract_path, contract_path.parent)
        results.append(parse_probe(backend, completed.stdout))
    return results


def run_all(contract_path: Path, contract: dict[str, Any], evidence: Path) -> dict[str, Any]:
    summary = validate_contract(contract)
    probes = probe_all(contract, contract_path)
    reservation = evidence_namespace.reserve_exact(evidence)
    result: dict[str, Any] = {
        "schema_version": 1,
        "contract_id": contract["contract_id"],
        "contract_sha256": experiment_contract.sha256_file(contract_path),
        "route_sha256": summary["route_sha256"],
        "reservation_token": reservation["token"],
        "probes": probes,
        "backends": [],
    }
    try:
        for backend in sorted(contract["backends"], key=lambda item: item["id"]):
            backend_dir = evidence / backend["id"]
            backend_dir.mkdir()
            completed = run_process(backend, "run", contract_path, backend_dir)
            (backend_dir / "stdout.log").write_text(completed.stdout, encoding="utf-8")
            (backend_dir / "stderr.log").write_text(completed.stderr, encoding="utf-8")
            if backend["adapter"] == "redux-events-v1":
                records = normalize_redux(completed.stdout, backend["id"])
            else:
                records = parse_normalized(
                    completed.stdout.splitlines(), backend["id"])
            validate_records(
                records, backend["id"], contract["route"]["target_relative_frame"])
            normalized = backend_dir / "observations.jsonl"
            normalized.write_text("".join(
                json.dumps(record, sort_keys=True, separators=(",", ":")) + "\n"
                for record in records), encoding="utf-8")
            result["backends"].append({
                "id": backend["id"], "adapter": backend["adapter"],
                "records": len(records),
                "observations": str(normalized.resolve()),
                "observations_sha256": experiment_contract.sha256_file(normalized),
            })
        result["ok"] = True
    except (OSError, OracleError) as error:
        result["ok"] = False
        result["error"] = str(error)
        write_json(evidence / "run.json", result)
        raise
    write_json(evidence / "run.json", result)
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("contract", type=Path)
    parser.add_argument("--var", action="append", default=[], metavar="NAME=VALUE")
    parser.add_argument("--probe-only", action="store_true")
    parser.add_argument("--evidence", type=Path)
    args = parser.parse_args()
    try:
        contract = load_contract(args.contract, args.var)
        preflight = validate_contract(contract)
        if args.probe_only:
            result = {**preflight, "probes": probe_all(contract, args.contract)}
        else:
            if args.evidence is None:
                parser.error("--evidence is required unless --probe-only is used")
            result = run_all(args.contract, contract, args.evidence)
    except (OSError, json.JSONDecodeError, OracleError,
            evidence_namespace.NamespaceError) as error:
        print(json.dumps({"ok": False, "error": str(error)}, sort_keys=True))
        return 2
    print(json.dumps(result, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
