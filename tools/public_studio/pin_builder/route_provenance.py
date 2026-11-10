#!/usr/bin/env python3
"""Validate local route inputs and emit path-free provenance receipts."""

from __future__ import annotations

import hashlib
import json
import re
from pathlib import Path
from typing import Any


SHA256_RE = re.compile(r"[0-9a-f]{64}\Z")
INPUT_KINDS = {
    "input_movie", "save_state", "route_contract", "checkpoint",
    "content_binary",
}
REDISTRIBUTION = {"allowed", "prohibited", "unknown"}


class ProvenanceError(ValueError):
    """A route input lacks required local provenance."""


def file_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def object_sha256(value: Any) -> str:
    body = json.dumps(value, sort_keys=True, separators=(",", ":")).encode()
    return hashlib.sha256(body).hexdigest()


def _text(value: Any, label: str) -> str:
    if not isinstance(value, str) or not value.strip():
        raise ProvenanceError(f"{label} is required")
    return value.strip()


def validate(route_set: dict[str, Any], project_id: str) -> dict[str, Any]:
    if route_set.get("schema_version") != 1:
        raise ProvenanceError("route-set schema_version must be 1")
    if route_set.get("project_id") != project_id:
        raise ProvenanceError("route-set project_id does not match the binding")
    routes = route_set.get("routes")
    if not isinstance(routes, list) or not routes:
        raise ProvenanceError("route-set routes must be a nonempty array")
    route_ids: set[str] = set()
    safe_routes = []
    for route_index, route in enumerate(routes):
        if not isinstance(route, dict):
            raise ProvenanceError(f"routes[{route_index}] must be an object")
        route_id = _text(route.get("route_id"), f"routes[{route_index}].route_id")
        if route_id in route_ids:
            raise ProvenanceError(f"duplicate route_id: {route_id}")
        route_ids.add(route_id)
        inputs = route.get("inputs")
        if not isinstance(inputs, list) or not inputs:
            raise ProvenanceError(f"{route_id} requires local provenance inputs")
        input_ids: set[str] = set()
        safe_inputs = []
        for input_index, item in enumerate(inputs):
            label = f"{route_id}.inputs[{input_index}]"
            if not isinstance(item, dict):
                raise ProvenanceError(f"{label} must be an object")
            input_id = _text(item.get("id"), f"{label}.id")
            if input_id in input_ids:
                raise ProvenanceError(f"{route_id} has duplicate input id: {input_id}")
            input_ids.add(input_id)
            kind = item.get("kind")
            if kind not in INPUT_KINDS:
                raise ProvenanceError(f"{label}.kind is invalid")
            path = Path(_text(item.get("path"), f"{label}.path")).resolve()
            if not path.is_file():
                raise ProvenanceError(f"{label} file is absent")
            expected = str(item.get("sha256", "")).lower()
            if SHA256_RE.fullmatch(expected) is None:
                raise ProvenanceError(f"{label}.sha256 must be a SHA-256 digest")
            actual = file_sha256(path)
            if actual != expected:
                raise ProvenanceError(f"{label} content changed")
            size = item.get("bytes")
            if size != path.stat().st_size:
                raise ProvenanceError(f"{label} size changed")
            redistribution = item.get("redistribution")
            if redistribution not in REDISTRIBUTION:
                raise ProvenanceError(f"{label}.redistribution is invalid")
            safe_inputs.append({
                "id": input_id,
                "kind": kind,
                "sha256": actual,
                "bytes": size,
                "format": _text(item.get("format"), f"{label}.format"),
                "source": _text(item.get("source"), f"{label}.source"),
                "license": _text(item.get("license"), f"{label}.license"),
                "redistribution": redistribution,
                "local_only": redistribution != "allowed" or item.get("local_only") is True,
            })
        safe_routes.append({
            "route_id": route_id,
            "route_sha256": object_sha256({
                "route_id": route_id,
                "inputs": safe_inputs,
                "contract_sha256": route.get("contract_sha256"),
            }),
            "inputs": safe_inputs,
        })
    receipt = {
        "schema_version": 1,
        "project_id": project_id,
        "routes": safe_routes,
        "paths_published": False,
        "bodies_published": False,
    }
    receipt["receipt_sha256"] = object_sha256(receipt)
    return receipt
