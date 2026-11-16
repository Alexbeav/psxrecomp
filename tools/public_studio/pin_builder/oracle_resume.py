#!/usr/bin/env python3
"""Create and verify immutable oracle route segments and continuation chains."""

from __future__ import annotations

import argparse
import hashlib
import json
import sys
from pathlib import Path
from typing import Any


TOOLS = Path(__file__).resolve().parent
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))

import evidence_namespace  # noqa: E402
import experiment_contract  # noqa: E402
import oracle_backend_adapter  # noqa: E402
import oracle_runner  # noqa: E402


class ResumeError(ValueError):
    """A segment or continuation chain is invalid."""


def load_object(path: Path) -> dict[str, Any]:
    value = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        raise ResumeError(f"JSON root must be an object: {path}")
    return value


def write_new(path: Path, value: Any) -> None:
    if path.exists():
        raise ResumeError(f"output already exists: {path}")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def artifact(path: Path) -> dict[str, str]:
    resolved = path.resolve()
    if not resolved.is_file():
        raise ResumeError(f"artifact is absent: {resolved}")
    return {
        "path": str(resolved),
        "sha256": experiment_contract.sha256_file(resolved),
    }


def verify_artifact(value: Any, label: str) -> Path:
    if not isinstance(value, dict):
        raise ResumeError(f"{label} is invalid")
    path = Path(str(value.get("path", "")))
    if not path.is_file():
        raise ResumeError(f"{label} is absent: {path}")
    actual = experiment_contract.sha256_file(path)
    if actual.lower() != str(value.get("sha256", "")).lower():
        raise ResumeError(f"{label} hash mismatch")
    return path


def load_records(path: Path, expected_hash: str, backend_id: str,
                 start_frame: int, final_frame: int) -> list[dict[str, Any]]:
    actual = experiment_contract.sha256_file(path)
    if actual.lower() != expected_hash.lower():
        raise ResumeError("observation stream hash mismatch")
    records: list[dict[str, Any]] = []
    for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        if line.startswith(oracle_runner.ORACLE_PREFIX):
            line = line[len(oracle_runner.ORACLE_PREFIX):]
        try:
            value = json.loads(line)
        except json.JSONDecodeError as error:
            raise ResumeError(f"observation line {number} is invalid JSON") from error
        if not isinstance(value, dict):
            raise ResumeError(f"observation line {number} is not an object")
        records.append(value)
    try:
        oracle_runner.validate_records(records, backend_id, final_frame, start_frame)
    except oracle_runner.OracleError as error:
        raise ResumeError(str(error)) from error
    return records


def validate_terminal_link(records: list[dict[str, Any]], terminal: dict[str, Any]) -> None:
    checkpoints = [item for item in records if item.get("record") == "checkpoint"]
    final = checkpoints[-1]
    linked = final.get("checkpoint")
    if not isinstance(linked, dict):
        raise ResumeError("final observation does not link the terminal checkpoint")
    if str(linked.get("sha256", "")).lower() != str(terminal.get("sha256", "")).lower():
        raise ResumeError("terminal checkpoint link hash mismatch")


def create_receipt(
    output: Path, segment_id: str, contract_path: Path, profile_path: Path,
    observations_path: Path, terminal_checkpoint_path: Path,
    start_frame: int, final_frame: int, parent_path: Path | None,
) -> dict[str, Any]:
    if start_frame < 0 or final_frame < start_frame:
        raise ResumeError("segment frame bounds are invalid")
    contract = load_object(contract_path)
    if contract.get("schema_version") != 1:
        raise ResumeError("contract schema is invalid")
    route = contract.get("route")
    if not isinstance(route, dict):
        raise ResumeError("contract route is absent")
    actual_route_hash = experiment_contract.route_sha256(route)
    if actual_route_hash.lower() != str(route.get("sha256", "")).lower():
        raise ResumeError("contract route hash mismatch")
    profile = load_object(profile_path)
    summary = oracle_backend_adapter.validate_profile(profile)
    terminal = artifact(terminal_checkpoint_path)
    observations = artifact(observations_path)
    records = load_records(
        observations_path, observations["sha256"], summary["backend_id"],
        start_frame, final_frame)
    validate_terminal_link(records, terminal)
    parent_hash: str | None = None
    initial_hash: str | None = None
    if parent_path is not None:
        parent = load_object(parent_path)
        validate_receipt(parent, parent_path)
        parent_hash = experiment_contract.sha256_file(parent_path)
        initial_hash = parent["terminal_checkpoint"]["sha256"]
        if start_frame != parent["final_frame"] + 1:
            raise ResumeError("continued segment does not start after its parent")
        for key, expected in (
            ("contract_id", contract.get("contract_id")),
            ("route_sha256", actual_route_hash),
            ("backend_id", summary["backend_id"]),
            ("profile_sha256", experiment_contract.sha256_file(profile_path)),
            ("backend_sha256", summary["backend_sha256"]),
        ):
            if str(parent.get(key, "")).lower() != str(expected).lower():
                raise ResumeError(f"continued segment {key} differs from its parent")
    receipt = {
        "$schema": "../schema/oracle-segment-receipt.schema.json",
        "schema_version": 1,
        "status": "complete",
        "segment_id": segment_id,
        "contract_id": contract.get("contract_id"),
        "contract_sha256": experiment_contract.sha256_file(contract_path),
        "route_sha256": actual_route_hash,
        "backend_id": summary["backend_id"],
        "profile_sha256": experiment_contract.sha256_file(profile_path),
        "backend_sha256": summary["backend_sha256"],
        "parent_receipt_sha256": parent_hash,
        "start_frame": start_frame,
        "final_frame": final_frame,
        "initial_checkpoint_sha256": initial_hash,
        "terminal_checkpoint": terminal,
        "observations": observations,
    }
    write_new(output, receipt)
    return receipt


def validate_receipt(value: dict[str, Any], source: Path | None = None) -> None:
    if value.get("schema_version") != 1:
        raise ResumeError("segment receipt schema is invalid")
    if value.get("status") != "complete":
        raise ResumeError("only a complete segment can enter a chain")
    start = value.get("start_frame")
    final = value.get("final_frame")
    if not isinstance(start, int) or not isinstance(final, int) or final < start:
        raise ResumeError("segment receipt frame bounds are invalid")
    stream = verify_artifact(value.get("observations"), "observation stream")
    terminal = value.get("terminal_checkpoint")
    verify_artifact(terminal, "terminal checkpoint")
    records = load_records(
        stream, value["observations"]["sha256"], value.get("backend_id"),
        start, final)
    validate_terminal_link(records, terminal)
    if source is not None and not source.is_file():
        raise ResumeError("segment receipt source is absent")


def combined_hash(record_groups: list[list[dict[str, Any]]]) -> str:
    digest = hashlib.sha256()
    for records in record_groups:
        for record in records:
            if record.get("record") != "checkpoint":
                continue
            digest.update(json.dumps(
                record, sort_keys=True, separators=(",", ":"),
                ensure_ascii=False).encode("utf-8"))
            digest.update(b"\n")
    return digest.hexdigest()


def build_chain(receipt_paths: list[Path]) -> dict[str, Any]:
    if not receipt_paths:
        raise ResumeError("a chain needs at least one segment")
    entries: list[dict[str, Any]] = []
    records: list[list[dict[str, Any]]] = []
    previous: dict[str, Any] | None = None
    previous_hash: str | None = None
    first: dict[str, Any] | None = None
    for path in receipt_paths:
        receipt = load_object(path)
        validate_receipt(receipt, path)
        receipt_hash = experiment_contract.sha256_file(path)
        if first is None:
            first = receipt
            if receipt.get("parent_receipt_sha256") is not None:
                raise ResumeError("the first chain segment has a parent")
            if receipt.get("initial_checkpoint_sha256") is not None:
                raise ResumeError("the first chain segment has an initial checkpoint")
        else:
            assert previous is not None and previous_hash is not None
            if receipt.get("parent_receipt_sha256", "").lower() != previous_hash.lower():
                raise ResumeError("segment parent receipt hash mismatch")
            if receipt["start_frame"] != previous["final_frame"] + 1:
                raise ResumeError("segment frames are not contiguous")
            if receipt.get("initial_checkpoint_sha256", "").lower() != previous["terminal_checkpoint"]["sha256"].lower():
                raise ResumeError("segment initial checkpoint does not match its parent")
            for key in ("contract_id", "contract_sha256", "route_sha256", "backend_id",
                        "profile_sha256", "backend_sha256"):
                if str(receipt.get(key, "")).lower() != str(first.get(key, "")).lower():
                    raise ResumeError(f"segment chain changes {key}")
        stream = Path(receipt["observations"]["path"])
        records.append(load_records(
            stream, receipt["observations"]["sha256"], receipt["backend_id"],
            receipt["start_frame"], receipt["final_frame"]))
        entries.append({
            "receipt": str(path.resolve()),
            "receipt_sha256": receipt_hash,
            "start_frame": receipt["start_frame"],
            "final_frame": receipt["final_frame"],
            "observations_sha256": receipt["observations"]["sha256"],
        })
        previous = receipt
        previous_hash = receipt_hash
    assert first is not None and previous is not None
    return {
        "$schema": "../schema/oracle-segment-chain.schema.json",
        "schema_version": 1,
        "ok": True,
        "contract_id": first["contract_id"],
        "route_sha256": first["route_sha256"],
        "backend_id": first["backend_id"],
        "start_frame": first["start_frame"],
        "final_frame": previous["final_frame"],
        "segments": entries,
        "combined_observations_sha256": combined_hash(records),
    }


def plan_next(parent_path: Path, evidence_parent: Path, stem: str) -> dict[str, Any]:
    parent = load_object(parent_path)
    validate_receipt(parent, parent_path)
    reservation = evidence_namespace.reserve_next(evidence_parent, stem)
    return {
        "schema_version": 1,
        "status": "reserved",
        "namespace": reservation["evidence_root"],
        "reservation_token": reservation["token"],
        "parent_receipt": str(parent_path.resolve()),
        "parent_receipt_sha256": experiment_contract.sha256_file(parent_path),
        "start_frame": parent["final_frame"] + 1,
        "initial_checkpoint": parent["terminal_checkpoint"],
        "contract_id": parent["contract_id"],
        "route_sha256": parent["route_sha256"],
        "backend_id": parent["backend_id"],
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    record = sub.add_parser("record")
    record.add_argument("output", type=Path)
    record.add_argument("--segment-id", required=True)
    record.add_argument("--contract", type=Path, required=True)
    record.add_argument("--profile", type=Path, required=True)
    record.add_argument("--observations", type=Path, required=True)
    record.add_argument("--terminal-checkpoint", type=Path, required=True)
    record.add_argument("--start-frame", type=int, required=True)
    record.add_argument("--final-frame", type=int, required=True)
    record.add_argument("--parent", type=Path)
    chain = sub.add_parser("chain")
    chain.add_argument("output", type=Path)
    chain.add_argument("receipts", type=Path, nargs="+")
    next_parser = sub.add_parser("next")
    next_parser.add_argument("parent", type=Path)
    next_parser.add_argument("evidence_parent", type=Path)
    next_parser.add_argument("stem")
    next_parser.add_argument("output", type=Path)
    args = parser.parse_args()
    try:
        if args.command == "record":
            result = create_receipt(
                args.output, args.segment_id, args.contract, args.profile,
                args.observations, args.terminal_checkpoint,
                args.start_frame, args.final_frame, args.parent)
        elif args.command == "chain":
            result = build_chain(args.receipts)
            write_new(args.output, result)
        else:
            result = plan_next(args.parent, args.evidence_parent, args.stem)
            write_new(args.output, result)
    except (ResumeError, evidence_namespace.NamespaceError,
            oracle_backend_adapter.AdapterError, OSError, json.JSONDecodeError) as error:
        print(json.dumps({"ok": False, "error": str(error)}, sort_keys=True))
        return 2
    print(json.dumps({"ok": True, "result": result}, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
