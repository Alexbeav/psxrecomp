#!/usr/bin/env python3
"""Freeze hash-bound Workbench component seams without copying private inputs."""

from __future__ import annotations

import hashlib
import json
from pathlib import Path
from typing import Any

try:
    from .local_input_binding import validate_binding
except ImportError:
    from local_input_binding import validate_binding  # type: ignore


class FreezeError(ValueError):
    """The requested component set cannot be frozen safely."""


def _load(path: Path) -> dict[str, Any]:
    value = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        raise FreezeError(f"{path} is not a JSON object")
    return value


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def _verified_file(item: dict[str, Any], label: str) -> tuple[Path, str]:
    path = Path(str(item.get("path", ""))).resolve()
    expected = str(item.get("sha256", "")).lower()
    if not path.is_file():
        raise FreezeError(f"{label} is absent")
    actual = _sha256(path)
    if actual != expected:
        raise FreezeError(f"{label} hash mismatch")
    return path, actual


def _digest_object(value: dict[str, Any]) -> str:
    body = json.dumps(value, sort_keys=True, separators=(",", ":")).encode()
    return hashlib.sha256(body).hexdigest()


def freeze(local_binding: Path, route_set_path: Path, output: Path) -> dict[str, Any]:
    """Validate and freeze component identities and their data-contract seams."""
    if output.exists():
        raise FreezeError(f"output already exists: {output}")
    binding = _load(local_binding.resolve())
    binding_receipt = validate_binding(binding)
    route_set_path = route_set_path.resolve()
    route_set = _load(route_set_path)
    if route_set.get("schema_version") != 1:
        raise FreezeError("route set schema_version must be 1")
    if route_set.get("project_id") != binding_receipt["project_id"]:
        raise FreezeError("route set project does not match binding")

    profile_path = Path(str(route_set.get("backend_profile", ""))).resolve()
    profile = _load(profile_path)
    if profile.get("schema_version") != 1:
        raise FreezeError("backend profile schema_version must be 1")

    components = []
    for component in profile.get("components", []):
        if not isinstance(component, dict):
            raise FreezeError("backend component is invalid")
        path, actual = _verified_file(component, f"component {component.get('id', '')}")
        components.append({
            "id": component.get("id"),
            "sha256": actual,
            "bytes": path.stat().st_size,
            "revision": component.get("revision"),
            "license": component.get("license"),
            "local_only": bool(component.get("local_only")),
        })

    stages = []
    for stage in route_set.get("pipeline", []):
        if not isinstance(stage, dict) or not isinstance(stage.get("command"), list) or not stage["command"]:
            raise FreezeError("pipeline stage is invalid")
        executable = Path(stage["command"][0]).resolve()
        actual = _sha256(executable) if executable.is_file() else ""
        if actual != str(stage.get("executable_sha256", "")).lower():
            raise FreezeError(f"pipeline stage {stage.get('id', '')} executable hash mismatch")
        stages.append({"id": stage.get("id"), "executable_sha256": actual})

    inputs = []
    for route in route_set.get("routes", []):
        if not isinstance(route, dict):
            raise FreezeError("route is invalid")
        contract = {"path": route.get("contract"), "sha256": route.get("contract_sha256")}
        contract_path, contract_hash = _verified_file(contract, f"route {route.get('route_id', '')} contract")
        route_inputs = [{
            "id": "contract", "sha256": contract_hash, "bytes": contract_path.stat().st_size,
            "kind": "route_contract", "local_only": True,
        }]
        for item in route.get("inputs", []):
            if not isinstance(item, dict):
                raise FreezeError("route input is invalid")
            path, actual = _verified_file(item, f"route input {item.get('id', '')}")
            route_inputs.append({
                "id": item.get("id"), "kind": item.get("kind"), "sha256": actual,
                "bytes": path.stat().st_size, "local_only": bool(item.get("local_only")),
            })
        inputs.append({"route_id": route.get("route_id"), "inputs": route_inputs})

    manifest: dict[str, Any] = {
        "schema_version": 1,
        "status": "frozen",
        "project_id": binding_receipt["project_id"],
        "binding": binding_receipt,
        "route_set": {"sha256": _sha256(route_set_path), "run_id": route_set.get("run_id")},
        "backend": {
            "profile_id": profile.get("profile_id"), "backend_id": profile.get("backend_id"),
            "profile_sha256": _sha256(profile_path), "normalizer": profile.get("normalizer"),
            "components": components,
        },
        "pipeline": stages,
        "routes": inputs,
        "seams": [
            {"producer": "local-binding", "consumer": "route-set", "contract": "local-input-binding-v1"},
            {"producer": "route-set", "consumer": "backend-adapter", "contract": "oracle-backend-profile-v1"},
            {"producer": "backend-adapter", "consumer": "determinism-gate", "contract": "normalized-jsonl-v1"},
            {"producer": "determinism-gate", "consumer": "resume-gate", "contract": "normalized-state-trace-v1"},
            {"producer": "runtime-trace", "consumer": "candidate-census", "contract": "support-evidence-only-v1"},
        ],
        "handling": {"contains_private_bodies": False, "contains_private_paths": False},
    }
    manifest["manifest_sha256"] = _digest_object(manifest)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return manifest
