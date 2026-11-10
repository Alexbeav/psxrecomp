#!/usr/bin/env python3
"""Adapt one pinned offline PSX backend to the Workbench oracle protocol."""

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

import experiment_contract  # noqa: E402
import evidence_namespace  # noqa: E402
import oracle_runner  # noqa: E402


class AdapterError(ValueError):
    """The backend profile, process, or evidence is invalid."""


TOKENS = {
    "BACKEND_EXECUTABLE", "CONTRACT", "EVIDENCE", "BACKEND",
    "START_FRAME", "TARGET_FRAME", "CHECKPOINT",
}


def component_token(component_id: str) -> str:
    """Return the stable process token for one pinned component."""
    return "COMPONENT_" + component_id.upper().replace("-", "_")


def load_object(path: Path) -> dict[str, Any]:
    value = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        raise AdapterError(f"JSON root must be an object: {path}")
    return value


def validate_profile(value: dict[str, Any]) -> dict[str, Any]:
    if value.get("schema_version") != 1:
        raise AdapterError("unsupported backend profile schema_version")
    backend_id = value.get("backend_id")
    if backend_id not in oracle_runner.BACKENDS:
        raise AdapterError("backend_id is invalid")
    capabilities = value.get("capabilities")
    if (not isinstance(capabilities, list) or not capabilities or
            len(capabilities) != len(set(capabilities))):
        raise AdapterError("capabilities must be a unique nonempty array")
    backend = value.get("backend")
    if not isinstance(backend, dict):
        raise AdapterError("backend must be an object")
    executable = Path(str(backend.get("executable", ""))).resolve()
    if not executable.is_file():
        raise AdapterError(f"backend executable is absent: {executable}")
    actual_hash = experiment_contract.sha256_file(executable)
    if actual_hash.lower() != str(backend.get("sha256", "")).lower():
        raise AdapterError("backend executable hash mismatch")
    components: dict[str, dict[str, Any]] = {}
    for component in value.get("components", []):
        if not isinstance(component, dict):
            raise AdapterError("component must be an object")
        component_id = component.get("id")
        if not isinstance(component_id, str) or not component_id:
            raise AdapterError("component id is invalid")
        if component_id in components:
            raise AdapterError(f"duplicate component id: {component_id}")
        path = Path(str(component.get("path", ""))).resolve()
        if not path.is_file():
            raise AdapterError(f"component is absent: {component_id}")
        digest = experiment_contract.sha256_file(path)
        if digest.lower() != str(component.get("sha256", "")).lower():
            raise AdapterError(f"component hash mismatch: {component_id}")
        required_component = {"source", "revision", "license", "local_only"}
        if not required_component <= set(component):
            raise AdapterError(f"component provenance is incomplete: {component_id}")
        components[component_id] = {**component, "path": str(path), "sha256": digest}
    for name in ("probe", "run"):
        process = value.get(name)
        if not isinstance(process, dict) or not process.get("command"):
            raise AdapterError(f"{name} process is absent")
        command = process["command"]
        if command[0] != "${BACKEND_EXECUTABLE}":
            raise AdapterError(f"{name} must launch the hashed backend executable")
        if not Path(str(process.get("working_directory", ""))).is_dir():
            raise AdapterError(f"{name} working directory is absent")
        timeout = process.get("timeout_seconds")
        if not isinstance(timeout, int) or timeout < 1:
            raise AdapterError(f"{name} timeout is invalid")
    normalizer = value.get("normalizer")
    if normalizer not in {"normalized-jsonl-v1", "redux-events-v1", "jsonl-file-v1"}:
        raise AdapterError("normalizer is invalid")
    if normalizer == "jsonl-file-v1" and not value.get("normalized_file"):
        raise AdapterError("jsonl-file-v1 requires normalized_file")
    provenance = value.get("provenance")
    required = {"source", "revision", "license", "local_only"}
    if not isinstance(provenance, dict) or not required <= set(provenance):
        raise AdapterError("provenance is incomplete")
    return {
        "backend_id": backend_id,
        "backend_executable": str(executable),
        "backend_sha256": actual_hash,
        "capabilities": sorted(capabilities),
        "profile_id": value.get("profile_id"),
        "components": [components[key] for key in sorted(components)],
        "provenance": provenance,
    }


def replace_tokens(value: str, replacements: dict[str, str]) -> str:
    result = value
    for name, replacement in replacements.items():
        result = result.replace("${" + name + "}", replacement)
    if "${" in result:
        raise AdapterError(f"unresolved process token: {result}")
    return result


def run_process(
    profile: dict[str, Any], name: str, contract: Path, evidence: Path,
    start_frame: int, target_frame: int, checkpoint: Path | None,
) -> subprocess.CompletedProcess[str]:
    backend = Path(profile["backend"]["executable"]).resolve()
    replacements = {
        "BACKEND_EXECUTABLE": str(backend),
        "CONTRACT": str(contract.resolve()),
        "EVIDENCE": str(evidence.resolve()),
        "BACKEND": profile["backend_id"],
        "START_FRAME": str(start_frame),
        "TARGET_FRAME": str(target_frame),
        "CHECKPOINT": "" if checkpoint is None else str(checkpoint.resolve()),
    }
    for component in profile.get("components", []):
        replacements[component_token(component["id"])] = str(
            Path(component["path"]).resolve())
    process = profile[name]
    command = [replace_tokens(item, replacements) for item in process["command"]]
    environment = os.environ.copy()
    environment.update({
        "PSXWB_ORACLE_CONTRACT": replacements["CONTRACT"],
        "PSXWB_ORACLE_EVIDENCE": replacements["EVIDENCE"],
        "PSXWB_ORACLE_BACKEND": replacements["BACKEND"],
        "PSXWB_ORACLE_START_FRAME": replacements["START_FRAME"],
        "PSXWB_ORACLE_TARGET_FRAME": replacements["TARGET_FRAME"],
        "PSXWB_ORACLE_CHECKPOINT": replacements["CHECKPOINT"],
    })
    environment.update({
        key: replace_tokens(value, replacements)
        for key, value in process.get("environment", {}).items()
    })
    try:
        completed = subprocess.run(
            command, cwd=process["working_directory"], env=environment,
            capture_output=True, text=True, encoding="utf-8", errors="replace",
            timeout=process["timeout_seconds"], check=False, shell=False)
    except subprocess.TimeoutExpired as error:
        raise AdapterError(f"{name} timed out") from error
    if completed.returncode != 0:
        raise AdapterError(f"{name} exited with {completed.returncode}")
    return completed


def parse_native_probe(profile: dict[str, Any], stdout: str) -> dict[str, Any]:
    lines = [line for line in stdout.splitlines() if line.strip()]
    if not lines:
        raise AdapterError("native probe emitted no terminal JSON")
    try:
        value = json.loads(lines[-1])
    except json.JSONDecodeError as error:
        raise AdapterError("native probe terminal line is not JSON") from error
    if not isinstance(value, dict):
        raise AdapterError("native probe terminal value is not an object")
    reported = value.get("capabilities")
    if not isinstance(reported, list):
        raise AdapterError("native probe capabilities are absent")
    missing = sorted(set(profile["capabilities"]) - set(reported))
    if missing:
        raise AdapterError(f"native probe did not confirm: {', '.join(missing)}")
    return value


def records_from_run(
    profile: dict[str, Any], completed: subprocess.CompletedProcess[str],
    evidence: Path,
) -> list[dict[str, Any]]:
    normalizer = profile["normalizer"]
    if normalizer == "redux-events-v1":
        return oracle_runner.normalize_redux(completed.stdout, profile["backend_id"])
    if normalizer == "jsonl-file-v1":
        relative = Path(profile["normalized_file"])
        if relative.is_absolute() or ".." in relative.parts:
            raise AdapterError("normalized_file must stay inside the evidence directory")
        path = evidence / relative
        if not path.is_file():
            raise AdapterError("backend normalized evidence file is absent")
        return oracle_runner.parse_normalized(path.read_text(encoding="utf-8").splitlines(), profile["backend_id"])
    return oracle_runner.parse_normalized(completed.stdout.splitlines(), profile["backend_id"])


def verify_record_artifacts(records: list[dict[str, Any]], evidence: Path) -> None:
    root = evidence.resolve()
    for record in records:
        for name in ("checkpoint", "trace", "render"):
            artifact = record.get(name)
            if artifact is None:
                continue
            if not isinstance(artifact, dict):
                raise AdapterError(f"{name} artifact must be an object")
            # A trace can be an inline normalized observation. Only a trace
            # object that declares a path is an evidence artifact.
            if name == "trace" and "path" not in artifact:
                continue
            path = Path(str(artifact.get("path", ""))).resolve()
            try:
                path.relative_to(root)
            except ValueError as error:
                raise AdapterError(f"{name} artifact escapes the evidence directory") from error
            if not path.is_file():
                raise AdapterError(f"{name} artifact is absent")
            actual = experiment_contract.sha256_file(path)
            if actual.lower() != str(artifact.get("sha256", "")).lower():
                raise AdapterError(f"{name} artifact hash mismatch")


def prepare_evidence(evidence: Path, reservation: Path | None) -> None:
    if reservation is None:
        try:
            evidence.mkdir(parents=True, exist_ok=False)
        except FileExistsError as error:
            raise AdapterError("evidence namespace already exists") from error
        return
    status = evidence_namespace.validate_receipt(reservation)
    if not status["ready"]:
        raise AdapterError("evidence reservation is not ready")
    if Path(status["evidence_root"]).resolve() != evidence.resolve():
        raise AdapterError("evidence reservation root mismatch")


def execute(
    operation: str, profile_path: Path, contract_path: Path, evidence: Path,
    start_frame: int = 0, target_frame: int | None = None,
    checkpoint: Path | None = None, reservation: Path | None = None,
) -> tuple[dict[str, Any], list[dict[str, Any]] | None]:
    profile = load_object(profile_path)
    summary = validate_profile(profile)
    contract = load_object(contract_path)
    route = contract.get("route", {})
    target = target_frame
    if target is None:
        target = int(route.get("target_relative_frame", -1))
    if start_frame < 0 or target < start_frame:
        raise AdapterError("frame bounds are invalid")
    if checkpoint is not None and not checkpoint.is_file():
        raise AdapterError("checkpoint is absent")
    prepare_evidence(evidence, reservation)
    completed = run_process(
        profile, operation, contract_path, evidence, start_frame, target, checkpoint)
    if operation == "probe":
        native = parse_native_probe(profile, completed.stdout)
        return ({
            "schema_version": 1, "backend_id": summary["backend_id"],
            "profile_id": summary["profile_id"],
            "backend_sha256": summary["backend_sha256"],
            "capabilities": summary["capabilities"],
            "components": summary["components"], "native_probe": native,
            "provenance": summary["provenance"],
        }, None)
    records = records_from_run(profile, completed, evidence)
    oracle_runner.validate_records(records, profile["backend_id"], target, start_frame)
    verify_record_artifacts(records, evidence)
    return (summary, records)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("operation", choices=("probe", "run"))
    parser.add_argument("--profile", type=Path, required=True)
    parser.add_argument("--contract", type=Path, required=True)
    parser.add_argument("--evidence", type=Path, required=True)
    parser.add_argument("--start-frame", type=int, default=0)
    parser.add_argument("--target-frame", type=int)
    parser.add_argument("--checkpoint", type=Path)
    parser.add_argument("--reservation", type=Path)
    args = parser.parse_args()
    try:
        summary, records = execute(
            args.operation, args.profile, args.contract, args.evidence,
            args.start_frame, args.target_frame, args.checkpoint, args.reservation)
        if args.operation == "probe":
            print(json.dumps(summary, sort_keys=True))
            return 0
        assert records is not None
        for record in records:
            print(oracle_runner.ORACLE_PREFIX + json.dumps(
                record, sort_keys=True, separators=(",", ":")))
        return 0
    except (AdapterError, oracle_runner.OracleError, OSError, json.JSONDecodeError) as error:
        print(json.dumps({"ok": False, "error": str(error)}, sort_keys=True))
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
