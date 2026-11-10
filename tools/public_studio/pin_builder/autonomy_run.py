#!/usr/bin/env python3
"""Authoritative offline Workbench autonomy route-set runner."""

from __future__ import annotations

import hashlib
import json
import os
import time
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

try:
    from . import (candidate_entry_census, deterministic_recorder,
                   evidence_namespace, experiment_contract,
                   generated_coverage, intervention_ledger, oracle_backend_adapter,
                   oracle_resume, route_provenance, session_monitor,
                   trace_seed_effect)
    from .local_input_binding import BindingError, validate_binding
except ImportError:
    import candidate_entry_census  # type: ignore
    import deterministic_recorder  # type: ignore
    import evidence_namespace  # type: ignore
    import experiment_contract  # type: ignore
    import generated_coverage  # type: ignore
    import intervention_ledger  # type: ignore
    import oracle_backend_adapter  # type: ignore
    import oracle_resume  # type: ignore
    import route_provenance  # type: ignore
    import session_monitor  # type: ignore
    import trace_seed_effect  # type: ignore
    from local_input_binding import BindingError, validate_binding


STAGES = (
    "disc_census",
    "static_discovery",
    "configuration_generation",
    "native_build",
    "boot_qualification",
)
POST_STAGES = ("fallback_and_ownership", "readiness_reporting")


class AutonomyError(ValueError):
    """The autonomy route set cannot run safely or completely."""


def load_object(path: Path) -> dict[str, Any]:
    value = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        raise AutonomyError(f"{path} is not a JSON object")
    return value


def write_json(path: Path, value: dict[str, Any]) -> None:
    if path.exists():
        raise AutonomyError(f"output already exists: {path}")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def file_sha256(path: Path) -> str:
    return experiment_contract.sha256_file(path)


def _safe_name(value: Any, label: str) -> str:
    if not isinstance(value, str) or not value or any(
        character not in "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-"
        for character in value
    ):
        raise AutonomyError(f"{label} contains an invalid character")
    return value


def _inside(path: Path, root: Path) -> bool:
    try:
        path.resolve().relative_to(root.resolve())
        return True
    except ValueError:
        return False


def _validate_hash_bound_command(stage: dict[str, Any], label: str) -> tuple[list[str], Path, float]:
    command = stage.get("command")
    if not isinstance(command, list) or not command or not all(
        isinstance(item, str) and item for item in command
    ):
        raise AutonomyError(f"{label}.command is invalid")
    executable = Path(command[0]).resolve()
    if not executable.is_file():
        raise AutonomyError(f"{label} executable is absent")
    expected = stage.get("executable_sha256")
    if file_sha256(executable).lower() != str(expected).lower():
        raise AutonomyError(f"{label} executable hash mismatch")
    working_directory = Path(str(stage.get("working_directory", ""))).resolve()
    if not working_directory.is_dir():
        raise AutonomyError(f"{label} working_directory is absent")
    timeout = stage.get("timeout_seconds")
    if not isinstance(timeout, (int, float)) or isinstance(timeout, bool) or timeout <= 0:
        raise AutonomyError(f"{label} timeout_seconds is invalid")
    return command, working_directory, float(timeout)


def _run_stage(stage: dict[str, Any], stage_id: str, stage_root: Path) -> dict[str, Any]:
    if stage.get("id") != stage_id:
        raise AutonomyError(f"expected stage {stage_id}")
    command, cwd, timeout = _validate_hash_bound_command(stage, stage_id)
    environment = os.environ.copy()
    additions = stage.get("environment", {})
    if not isinstance(additions, dict) or not all(
        isinstance(key, str) and isinstance(value, str) for key, value in additions.items()
    ):
        raise AutonomyError(f"{stage_id}.environment is invalid")
    environment.update(additions)
    failures = []
    for attempt in range(1, 4):
        receipt = session_monitor.run(
            command,
            cwd,
            stage_root / f"{stage_id}-attempt-{attempt}",
            timeout_seconds=timeout,
            environment=environment,
            heartbeat=(Path(stage["heartbeat"]).resolve() if stage.get("heartbeat") else None),
            stall_seconds=stage.get("stall_seconds"),
        )
        if receipt["classification"] == "clean_exit":
            receipt["attempt"] = attempt
            return receipt
        failures.append(receipt["classification"])
        if len(failures) == 3 and len(set(failures)) == 1:
            raise AutonomyError(
                f"{stage_id} stopped after three identical failures: {failures[0]}")
    raise AutonomyError(f"{stage_id} failed")


def _write_observations(path: Path, records: list[dict[str, Any]]) -> None:
    path.write_text("".join(
        deterministic_recorder.PREFIX
        + json.dumps(record, sort_keys=True, separators=(",", ":")) + "\n"
        for record in records
    ), encoding="utf-8")


def _execute_oracle(profile: Path, contract: Path, root: Path,
                    start_frame: int, target_frame: int,
                    checkpoint: Path | None = None) -> dict[str, Any]:
    started = time.monotonic()
    reservation = evidence_namespace.reserve_exact(root)
    reservation_path = root.parent / (root.name + ".reservation.json")
    write_json(reservation_path, reservation)
    summary, records = oracle_backend_adapter.execute(
        "run", profile, contract, root, start_frame, target_frame,
        checkpoint, reservation_path,
    )
    if records is None:
        raise AutonomyError("recorder returned no normalized records")
    observations = root / "observations.jsonl"
    _write_observations(observations, records)
    receipt = {
        "schema_version": 1,
        "status": "complete",
        "backend": summary,
        "profile_sha256": file_sha256(profile),
        "contract_sha256": file_sha256(contract),
        "reservation_sha256": file_sha256(reservation_path),
        "evidence_root": str(root.resolve()),
        "observations": {
            "path": str(observations.resolve()),
            "sha256": file_sha256(observations),
            "record_count": len(records),
        },
        "wall_seconds": time.monotonic() - started,
    }
    if checkpoint is not None:
        receipt["initial_checkpoint"] = {
            "sha256": file_sha256(checkpoint),
            "bytes": checkpoint.stat().st_size,
        }
    receipt_path = root.parent / (root.name + ".run.json")
    write_json(receipt_path, receipt)
    receipt["receipt_path"] = str(receipt_path.resolve())
    receipt["receipt_sha256"] = file_sha256(receipt_path)
    return receipt


def _checkpoint_record(records: list[dict[str, Any]], frame: int) -> dict[str, Any]:
    matches = [record for record in records if record.get("record") == "checkpoint"
               and record.get("relative_frame") == frame]
    if len(matches) != 1:
        raise AutonomyError(f"expected one checkpoint record for frame {frame}")
    return matches[0]


def _exclude_intermediate_terminal_receipt(
        records: list[dict[str, Any]], split: int) -> None:
    """Remove evidence that exists only because an intermediate segment ended."""
    state = _checkpoint_record(records, split).get("state")
    if isinstance(state, dict):
        state.pop("ram_sha256", None)


def _qualify_resume(profile: Path, contract: Path, route_root: Path,
                    uninterrupted: Path, start: int, target: int,
                    split: int, initial_checkpoint: Path | None = None) -> dict[str, Any]:
    if not (start <= split < target):
        raise AutonomyError("resume_frame must be inside the route")
    first = _execute_oracle(
        profile, contract, route_root / "resume-segment-1", start, split,
        initial_checkpoint,
    )
    checkpoint = Path(first["evidence_root"]) / "terminal.state"
    first_segment_path = route_root / "resume-segment-1.segment.json"
    oracle_resume.create_receipt(
        first_segment_path, "segment-1", contract, profile,
        Path(first["observations"]["path"]), checkpoint, start, split, None,
    )
    second = _execute_oracle(
        profile, contract, route_root / "resume-segment-2", split + 1,
        target, checkpoint,
    )
    second_checkpoint = Path(second["evidence_root"]) / "terminal.state"
    second_segment_path = route_root / "resume-segment-2.segment.json"
    oracle_resume.create_receipt(
        second_segment_path, "segment-2", contract, profile,
        Path(second["observations"]["path"]), second_checkpoint,
        split + 1, target, first_segment_path,
    )
    chain = oracle_resume.build_chain([first_segment_path, second_segment_path])
    chain_path = route_root / "resume-chain.json"
    write_json(chain_path, chain)
    reference_records = deterministic_recorder.load_records(uninterrupted)
    combined = [
        *[record for record in deterministic_recorder.load_records(
            Path(first["observations"]["path"])) if record.get("record") == "checkpoint"],
        *[record for record in deterministic_recorder.load_records(
            Path(second["observations"]["path"])) if record.get("record") == "checkpoint"],
    ]
    # The split record is terminal in segment 1, so it has an additional RAM
    # receipt. The uninterrupted record at that same frame is not terminal.
    # Compare the emulated state fields while retaining the hash-bound segment
    # checkpoint and the final terminal RAM receipt as independent evidence.
    _exclude_intermediate_terminal_receipt(combined, split)
    reference = [record for record in reference_records if record.get("record") == "checkpoint"]
    domains_combined = deterministic_recorder.streams([
        {"record": "start", "backend_id": "pcsx-redux"}, *combined,
        {"record": "complete", "backend_id": "pcsx-redux", "relative_frame": target},
    ])
    domains_reference = deterministic_recorder.streams(reference_records)
    state_equal = domains_combined["state"] == domains_reference["state"]
    trace_equal = domains_combined["trace"] == domains_reference["trace"]
    return {
        "schema_version": 1,
        "status": "qualified" if state_equal and trace_equal else "failed",
        "parent_receipt_sha256": file_sha256(first_segment_path),
        "terminal_checkpoint_sha256": file_sha256(checkpoint),
        "chain_sha256": file_sha256(chain_path),
        "state_equal": state_equal,
        "trace_equal": trace_equal,
        "reference_checkpoint_count": len(reference),
        "combined_checkpoint_count": len(combined),
        "wall_seconds": first["wall_seconds"] + second["wall_seconds"],
        "initial_checkpoint_sha256": (
            file_sha256(initial_checkpoint) if initial_checkpoint is not None else None
        ),
    }


def _route(route: dict[str, Any], profile: Path, evidence_root: Path,
           project_id: str) -> dict[str, Any]:
    route_id = _safe_name(route.get("route_id"), "route_id")
    contract = Path(str(route.get("contract", ""))).resolve()
    if not contract.is_file() or file_sha256(contract) != str(route.get("contract_sha256", "")).lower():
        raise AutonomyError(f"{route_id} contract is absent or changed")
    contract_value = load_object(contract)
    route_value = contract_value.get("route", {})
    start = route.get("start_frame", 0)
    target = route.get("target_frame", route_value.get("target_relative_frame"))
    if not isinstance(start, int) or not isinstance(target, int) or start < 0 or target < start:
        raise AutonomyError(f"{route_id} frame bounds are invalid")
    initial_checkpoint = None
    checkpoint_value = route.get("initial_checkpoint")
    if checkpoint_value is not None:
        if not isinstance(checkpoint_value, dict):
            raise AutonomyError(f"{route_id} initial checkpoint is invalid")
        initial_checkpoint = Path(str(checkpoint_value.get("path", ""))).resolve()
        expected = str(checkpoint_value.get("sha256", "")).lower()
        if not initial_checkpoint.is_file() or file_sha256(initial_checkpoint) != expected:
            raise AutonomyError(f"{route_id} initial checkpoint is absent or changed")
    route_root = evidence_root / route_id
    route_root.mkdir(parents=True, exist_ok=False)
    runs = [
        _execute_oracle(
            profile, contract, route_root / f"determinism-{index}", start, target,
            initial_checkpoint,
        )
        for index in (1, 2)
    ]
    determinism = deterministic_recorder.qualify(
        Path(runs[0]["observations"]["path"]),
        Path(runs[1]["observations"]["path"]),
    )
    determinism_path = route_root / "determinism.json"
    write_json(determinism_path, determinism)
    result: dict[str, Any] = {
        "route_id": route_id,
        "contract_sha256": file_sha256(contract),
        "backend_id": "pcsx-redux",
        "runs": runs,
        "determinism": determinism,
    }
    resume_frame = route.get("resume_frame")
    if resume_frame is not None:
        result["resume"] = _qualify_resume(
            profile, contract, route_root,
            Path(runs[0]["observations"]["path"]), start, target, resume_frame,
            initial_checkpoint,
        )
    runtime_modules = route.get("runtime_trace_modules")
    if runtime_modules:
        if not isinstance(runtime_modules, list):
            raise AutonomyError("runtime_trace_modules must be an array")
        modules = []
        for module in runtime_modules:
            if not isinstance(module, dict):
                raise AutonomyError("runtime trace module must be an object")
            binary = Path(str(module.get("binary_path", ""))).resolve()
            if not binary.is_file():
                raise AutonomyError("runtime trace module binary is absent")
            modules.append({
                **module,
                "binary_path": str(binary),
                "sources": [{
                    "tool": "pcsx-redux",
                    "format": "runtime_trace_jsonl",
                    "path": runs[0]["observations"]["path"],
                    "evidence_class": "runtime_trace",
                    "producer": module.get("producer", module.get("module_id")),
                    "route_id": route_id,
                    "backend_id": "pcsx-redux",
                    "backend_receipt_sha256": runs[0]["receipt_sha256"],
                    "independence_group": "pcsx-redux-runtime-trace",
                }],
            })
        manifest_path = route_root / "runtime-trace-manifest.json"
        write_json(manifest_path, {
            "schema_version": 1, "project_id": project_id, "modules": modules,
        })
        census = candidate_entry_census.normalize(manifest_path)
        census_path = route_root / "runtime-trace-census.json"
        write_json(census_path, census)
        result["runtime_trace_census"] = {
            "path": str(census_path.resolve()),
            "sha256": file_sha256(census_path),
            "candidate_count": sum(
                len(module["candidates"]) for module in census["modules"]),
            "admission_policy": "normal-evidence-required",
        }
    census_manifest = route.get("candidate_manifest")
    if census_manifest and not runtime_modules:
        manifest_path = Path(str(census_manifest)).resolve()
        census = candidate_entry_census.normalize(manifest_path)
        census_path = route_root / "runtime-trace-census.json"
        write_json(census_path, census)
        result["runtime_trace_census"] = {
            "path": str(census_path.resolve()),
            "sha256": file_sha256(census_path),
        }
    comparison = route.get("trace_seed_comparison")
    if comparison:
        effect = trace_seed_effect.compare(load_object(Path(str(comparison)).resolve()))
        effect_path = route_root / "trace-seed-effect.json"
        write_json(effect_path, effect)
        result["trace_seed_effect"] = effect
    coverage_input = route.get("generated_coverage")
    if coverage_input:
        coverage = generated_coverage.reconcile(
            load_object(Path(str(coverage_input)).resolve()))
        coverage_path = route_root / "generated-coverage.json"
        write_json(coverage_path, coverage)
        result["generated_coverage"] = {
            "path": str(coverage_path.resolve()),
            "sha256": file_sha256(coverage_path),
            "status": coverage["status"],
            "overhead": coverage["overhead"],
        }
    return result


def run(binding_path: Path, route_set_path: Path) -> dict[str, Any]:
    binding = load_object(binding_path)
    binding_receipt = validate_binding(binding)
    route_set = load_object(route_set_path)
    provenance = route_provenance.validate(route_set, binding_receipt["project_id"])
    run_id = _safe_name(route_set.get("run_id"), "run_id")
    project = Path(binding["project"]["path"]).resolve()
    output = Path(str(route_set.get("output_root", project / "autonomy-runs" / run_id))).resolve()
    if not _inside(output, project):
        raise AutonomyError("autonomy output must stay inside the bound project")
    if output.exists():
        raise AutonomyError("autonomy output already exists")
    output.mkdir(parents=True)
    started = time.monotonic()
    ledger = intervention_ledger.create(
        run_id, ["workbench", "run", str(binding_path), str(route_set_path)], "declared-route-set")
    ledger_path = output / "intervention-ledger.json"
    report_path = output / "autonomy-report.json"
    try:
        intervention_ledger.sync(ledger_path, ledger)
        write_json(output / "route-provenance-receipt.json", provenance)
        intervention_ledger.record_phase(ledger, "route_provenance", "pass")
        intervention_ledger.sync(ledger_path, ledger)
        pipeline = route_set.get("pipeline")
        expected_stages = (*STAGES, *POST_STAGES)
        if not isinstance(pipeline, list) or [stage.get("id") for stage in pipeline
                                             if isinstance(stage, dict)] != list(expected_stages):
            raise AutonomyError("pipeline must declare every authoritative stage in order")
        stage_results: dict[str, Any] = {}
        for stage in pipeline[:len(STAGES)]:
            stage_id = stage["id"]
            intervention_ledger.record_phase(ledger, stage_id, "start")
            intervention_ledger.sync(ledger_path, ledger)
            stage_results[stage_id] = _run_stage(stage, stage_id, output / "sessions")
            intervention_ledger.record_phase(
                ledger, stage_id, "pass", machine_seconds=stage_results[stage_id]["wall_seconds"])
            intervention_ledger.sync(ledger_path, ledger)
        profile = Path(str(route_set.get("backend_profile", ""))).resolve()
        profile_value = load_object(profile)
        if profile_value.get("backend_id") != "pcsx-redux":
            raise AutonomyError("the first autonomy recorder must be pcsx-redux")
        route_results = [
            _route(route, profile, output / "evidence", binding_receipt["project_id"])
            for route in route_set["routes"]
        ]
        intervention_ledger.record_phase(ledger, "route_replay", "pass")
        intervention_ledger.sync(ledger_path, ledger)
        for stage in pipeline[len(STAGES):]:
            stage_id = stage["id"]
            intervention_ledger.record_phase(ledger, stage_id, "start")
            intervention_ledger.sync(ledger_path, ledger)
            stage_results[stage_id] = _run_stage(stage, stage_id, output / "sessions")
            intervention_ledger.record_phase(
                ledger, stage_id, "pass", machine_seconds=stage_results[stage_id]["wall_seconds"])
            intervention_ledger.sync(ledger_path, ledger)
        passed = all(
            route["determinism"]["qualified"]
            and route.get("resume", {"status": "qualified"})["status"] == "qualified"
            for route in route_results
        )
        stage_seconds = sum(
            value["wall_seconds"] for value in stage_results.values())
        recorder_seconds = sum(
            sum(run["wall_seconds"] for run in route["runs"])
            + route.get("resume", {}).get("wall_seconds", 0.0)
            for route in route_results
        )
        intervention_ledger.finalize(
            ledger, "complete" if passed else "failed", started,
            machine_seconds=stage_seconds + recorder_seconds,
        )
        intervention_ledger.sync(ledger_path, ledger)
        report = {
            "schema_version": 1,
            "run_id": run_id,
            "status": "qualified" if passed else "failed",
            "scope": "declared-route-set",
            "project_id": binding_receipt["project_id"],
            "binding_sha256": binding_receipt["binding_sha256"],
            "route_set_sha256": file_sha256(route_set_path),
            "provenance_receipt_sha256": file_sha256(output / "route-provenance-receipt.json"),
            "pipeline": stage_results,
            "routes": route_results,
            "interventions": ledger["interventions"],
            "intervention_total": ledger["intervention_total"],
            "time": ledger["time"],
            "agent_time": ledger["time"]["agent_seconds"],
            "compute_cost": ledger["compute_cost"],
            "abstentions": route_set.get("abstentions", []),
        }
        write_json(report_path, report)
        return report | {"report_path": str(report_path.resolve())}
    except Exception as error:
        intervention_ledger.record_phase(ledger, "autonomy_run", "fail", str(error))
        intervention_ledger.finalize(ledger, "failed", started)
        intervention_ledger.sync(ledger_path, ledger)
        failure = {
            "schema_version": 1,
            "run_id": run_id,
            "status": "failed",
            "error": str(error),
            "intervention_total": ledger["intervention_total"],
        }
        if not report_path.exists():
            write_json(report_path, failure)
        raise
