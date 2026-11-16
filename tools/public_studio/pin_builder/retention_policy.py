#!/usr/bin/env python3
"""Create a deterministic, non-mutating Workbench retention plan."""

from __future__ import annotations

import hashlib
import json
import os
from datetime import datetime, timezone
from pathlib import Path, PurePath
from typing import Any


class RetentionPolicyError(RuntimeError):
    pass


DURABLE_CLASSES = {
    "claim", "control", "input", "manifest", "manual", "receipt",
    "report", "source", "worktree",
}
COLD_CLASSES = {
    "analysis", "attempt", "attempts", "build", "cache", "evidence",
    "extraction", "package", "qualification", "staging",
}
DURABLE_NAMES = {
    ".agent", "artifacts.json", "claims.json", "current_state.md",
    "decisions.md", "live.md", "runs.jsonl",
}
DURABLE_NAME_FRAGMENTS = ("claim", "manifest", "receipt", "report")


def _hash(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _reparse(path: Path) -> bool:
    try:
        attributes = getattr(path.lstat(), "st_file_attributes", 0)
    except OSError as error:
        raise RetentionPolicyError(f"cannot stat {path}: {error}") from error
    return path.is_symlink() or bool(attributes & 0x400)


def _relative_path(value: Any) -> PurePath:
    if not isinstance(value, str) or not value.strip():
        raise RetentionPolicyError("artifact path must be a non-empty string")
    path = PurePath(value)
    if path.is_absolute() or ".." in path.parts:
        raise RetentionPolicyError(f"artifact path must stay relative: {value}")
    return path


def _durable_path(path: PurePath) -> bool:
    lowered = [part.casefold() for part in path.parts]
    if any(part in DURABLE_NAMES for part in lowered):
        return True
    return any(fragment in path.name.casefold()
               for fragment in DURABLE_NAME_FRAGMENTS)


def _scan(root: Path, relative: PurePath, cold: bool) -> dict[str, Any]:
    target = (root / relative).resolve(strict=True)
    try:
        target.relative_to(root)
    except ValueError as error:
        raise RetentionPolicyError(f"artifact escapes namespace: {relative}") from error
    if not target.is_dir() or _reparse(target):
        raise RetentionPolicyError(f"artifact is not a regular directory: {relative}")
    files = 0
    total_bytes = 0
    newest_ns = 0
    for current, directories, names in os.walk(target, topdown=True,
                                                followlinks=False):
        current_path = Path(current)
        safe_directories = []
        for name in sorted(directories, key=str.casefold):
            child = current_path / name
            child_relative = child.relative_to(target)
            if _reparse(child):
                raise RetentionPolicyError(
                    f"reparse point in artifact {relative}: {child_relative.as_posix()}")
            if name == ".git":
                raise RetentionPolicyError(
                    f"Git worktree metadata in artifact {relative}")
            if cold and _durable_path(child_relative):
                raise RetentionPolicyError(
                    f"durable state in cold artifact {relative}: {child_relative.as_posix()}")
            safe_directories.append(name)
        directories[:] = safe_directories
        for name in sorted(names, key=str.casefold):
            path = current_path / name
            child_relative = path.relative_to(target)
            if _reparse(path) or not path.is_file():
                raise RetentionPolicyError(
                    f"non-regular entry in artifact {relative}: {child_relative.as_posix()}")
            if name == ".git":
                raise RetentionPolicyError(
                    f"Git worktree metadata in artifact {relative}")
            if cold and _durable_path(child_relative):
                raise RetentionPolicyError(
                    f"durable state in cold artifact {relative}: {child_relative.as_posix()}")
            stat = path.stat()
            files += 1
            total_bytes += stat.st_size
            newest_ns = max(newest_ns, stat.st_mtime_ns)
    return {
        "path": relative.as_posix(),
        "files": files,
        "bytes": total_bytes,
        "newest_mtime_ns": newest_ns,
    }


def _utc_instant(value: str) -> datetime:
    try:
        parsed = datetime.fromisoformat(value.replace("Z", "+00:00"))
    except (AttributeError, ValueError) as error:
        raise RetentionPolicyError(
            "evaluated_at_utc must be an ISO-8601 UTC instant") from error
    if parsed.tzinfo is None or parsed.utcoffset() != timezone.utc.utcoffset(parsed):
        raise RetentionPolicyError("evaluated_at_utc must use UTC")
    return parsed.astimezone(timezone.utc)


def plan(namespace: Path, inventory_path: Path, *, evaluated_at_utc: str,
         min_age_days: float = 0, min_size_bytes: int = 0) -> dict[str, Any]:
    if min_age_days < 0 or min_size_bytes < 0:
        raise RetentionPolicyError("retention thresholds cannot be negative")
    namespace = namespace.resolve(strict=True)
    inventory_path = inventory_path.resolve(strict=True)
    if not namespace.is_dir() or _reparse(namespace):
        raise RetentionPolicyError("namespace must be a regular directory")
    try:
        inventory = json.loads(inventory_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise RetentionPolicyError(f"cannot read inventory: {error}") from error
    if inventory.get("schema_version") != 1:
        raise RetentionPolicyError("inventory schema_version must be 1")
    artifacts = inventory.get("artifacts")
    if not isinstance(artifacts, list) or not artifacts:
        raise RetentionPolicyError("inventory must contain artifacts")
    parsed: list[tuple[str, PurePath]] = []
    for artifact in artifacts:
        if not isinstance(artifact, dict):
            raise RetentionPolicyError("artifact entries must be objects")
        artifact_class = artifact.get("class")
        if artifact_class not in DURABLE_CLASSES | COLD_CLASSES:
            raise RetentionPolicyError(
                f"unknown artifact class: {artifact_class}")
        parsed.append((str(artifact_class), _relative_path(artifact.get("path"))))
    paths = [path for _, path in parsed]
    if len({path.as_posix().casefold() for path in paths}) != len(paths):
        raise RetentionPolicyError("artifact paths must be unique")
    for left in paths:
        for right in paths:
            if left != right and left in right.parents:
                raise RetentionPolicyError("nested artifact roots are not allowed")
    evaluated_at = _utc_instant(evaluated_at_utc)
    now_ns = int(evaluated_at.timestamp() * 1_000_000_000)
    rows = []
    for artifact_class, relative in parsed:
        cold = artifact_class in COLD_CLASSES
        row = _scan(namespace, relative, cold)
        age_days = ((now_ns - row["newest_mtime_ns"]) / 1_000_000_000 / 86400
                    if row["newest_mtime_ns"] else 0)
        eligible = (cold and row["bytes"] >= min_size_bytes
                    and age_days >= min_age_days)
        rows.append({
            **row,
            "class": artifact_class,
            "age_days": round(age_days, 6),
            "disposition": "archive-candidate" if eligible else "retain-local",
            "reason": (
                "cold class meets thresholds" if eligible else
                "durable/hot class" if not cold else
                "cold class below age or size threshold"
            ),
        })
    rows.sort(key=lambda item: str(item["path"]).casefold())
    return {
        "schema_version": 1,
        "mode": "dry-run",
        "namespace": str(namespace),
        "inventory": str(inventory_path),
        "inventory_sha256": _hash(inventory_path),
        "evaluated_at_utc": evaluated_at.isoformat().replace("+00:00", "Z"),
        "thresholds": {
            "min_age_days": min_age_days,
            "min_size_bytes": min_size_bytes,
        },
        "artifacts": rows,
        "requirements": {
            "archive_before_remove": True,
            "byte_hash_verification": True,
            "recovery_receipt": True,
            "explicit_remove_authorization": True,
        },
        "source_removal_authorized": False,
    }
