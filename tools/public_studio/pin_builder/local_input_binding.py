#!/usr/bin/env python3
"""Create and validate local-only Workbench input bindings."""

from __future__ import annotations

import hashlib
import json
import re
from pathlib import Path
from typing import Any


SHA256_RE = re.compile(r"[0-9a-fA-F]{64}\Z")


class BindingError(ValueError):
    """A local input binding is invalid."""


def file_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def object_sha256(value: dict[str, Any]) -> str:
    body = json.dumps(value, sort_keys=True, separators=(",", ":")).encode()
    return hashlib.sha256(body).hexdigest()


def _expected(value: str | None, label: str) -> str | None:
    if value is None:
        return None
    if SHA256_RE.fullmatch(value) is None:
        raise BindingError(f"{label} must be a SHA-256 digest")
    return value.lower()


def create_binding(project: Path, disc: Path, bios: Path,
                   project_id: str | None = None,
                   disc_sha256: str | None = None,
                   bios_sha256: str | None = None) -> tuple[dict[str, Any], dict[str, Any]]:
    project = project.resolve()
    disc = disc.resolve()
    bios = bios.resolve()
    if not project.is_dir():
        raise BindingError("project must be an existing directory")
    if not disc.is_file():
        raise BindingError("disc must be an existing file")
    if not bios.is_file():
        raise BindingError("BIOS must be an existing file")
    identity = project_id or project.name
    if not identity:
        raise BindingError("project_id must be a nonempty string")
    actual_disc = file_sha256(disc)
    actual_bios = file_sha256(bios)
    expected_disc = _expected(disc_sha256, "disc_sha256")
    expected_bios = _expected(bios_sha256, "bios_sha256")
    if expected_disc is not None and expected_disc != actual_disc:
        raise BindingError("disc SHA-256 does not match")
    if expected_bios is not None and expected_bios != actual_bios:
        raise BindingError("BIOS SHA-256 does not match")
    binding = {
        "schema_version": 1,
        "project": {"id": identity, "path": str(project)},
        "disc": {"path": str(disc), "sha256": actual_disc,
                 "bytes": disc.stat().st_size},
        "bios": {"path": str(bios), "sha256": actual_bios,
                 "bytes": bios.stat().st_size},
        "handling": {
            "local_only": True,
            "proprietary": True,
            "publish_paths": False,
            "publish_bodies": False,
        },
    }
    receipt = {
        "schema_version": 1,
        "project_id": identity,
        "binding_sha256": object_sha256(binding),
        "disc": {"sha256": actual_disc, "bytes": disc.stat().st_size},
        "bios": {"sha256": actual_bios, "bytes": bios.stat().st_size},
        "handling": binding["handling"],
    }
    return binding, receipt


def validate_binding(value: dict[str, Any]) -> dict[str, Any]:
    if value.get("schema_version") != 1:
        raise BindingError("binding schema_version must be 1")
    handling = value.get("handling")
    if handling != {
        "local_only": True, "proprietary": True,
        "publish_paths": False, "publish_bodies": False,
    }:
        raise BindingError("binding handling policy must fail closed")
    project = value.get("project", {})
    if not isinstance(project.get("id"), str) or not project["id"]:
        raise BindingError("binding project.id is invalid")
    project_path = Path(str(project.get("path", ""))).resolve()
    if not project_path.is_dir():
        raise BindingError("bound project path is absent")
    for name in ("disc", "bios"):
        item = value.get(name, {})
        path = Path(str(item.get("path", ""))).resolve()
        expected = _expected(item.get("sha256"), f"binding {name}.sha256")
        if not path.is_file():
            raise BindingError(f"bound {name} path is absent")
        if file_sha256(path) != expected:
            raise BindingError(f"bound {name} content changed")
        if item.get("bytes") != path.stat().st_size:
            raise BindingError(f"bound {name} size changed")
    return {
        "schema_version": 1,
        "project_id": project["id"],
        "binding_sha256": object_sha256(value),
        "disc": {"sha256": value["disc"]["sha256"],
                 "bytes": value["disc"]["bytes"]},
        "bios": {"sha256": value["bios"]["sha256"],
                 "bytes": value["bios"]["bytes"]},
        "handling": handling,
    }
