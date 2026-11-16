#!/usr/bin/env python3
"""Reserve and validate an immutable evidence namespace."""

from __future__ import annotations

import argparse
import json
import os
import re
import secrets
from pathlib import Path
from typing import Any


MARKER_NAME = ".workbench-reservation.json"
STEM_RE = re.compile(r"[A-Za-z0-9][A-Za-z0-9._-]*\Z")


class NamespaceError(ValueError):
    """The namespace request or state is invalid."""


def write_json(path: Path, value: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def read_object(path: Path) -> dict[str, Any]:
    value = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        raise NamespaceError(f"{path} is not a JSON object")
    return value


def is_within(path: Path, root: Path) -> bool:
    try:
        path.resolve().relative_to(root.resolve())
        return True
    except ValueError:
        return False


def make_receipt(path: Path, token: str) -> dict[str, Any]:
    return {
        "schema_version": 1,
        "evidence_root": str(path.resolve()),
        "token": token,
        "process_id": os.getpid(),
    }


def reserve_exact(path: Path, token: str | None = None) -> dict[str, Any]:
    value = token or secrets.token_hex(16)
    if not value or any(character.isspace() for character in value):
        raise NamespaceError("token must be a nonempty string without spaces")
    try:
        path.mkdir(parents=True, exist_ok=False)
    except FileExistsError as error:
        raise NamespaceError(f"evidence namespace already exists: {path}") from error
    receipt = make_receipt(path, value)
    try:
        marker = path / MARKER_NAME
        with marker.open("x", encoding="utf-8") as stream:
            json.dump(receipt, stream, indent=2, sort_keys=True)
            stream.write("\n")
    except Exception:
        path.rmdir()
        raise
    return receipt


def reserve_next(parent: Path, stem: str,
                 token: str | None = None) -> dict[str, Any]:
    if STEM_RE.fullmatch(stem) is None or stem in {".", ".."}:
        raise NamespaceError("stem contains an invalid character")
    parent.mkdir(parents=True, exist_ok=True)
    revision = 1
    while True:
        candidate = parent / f"{stem}-r{revision}"
        try:
            return reserve_exact(candidate, token)
        except NamespaceError:
            if not candidate.exists():
                raise
            revision += 1


def validate_receipt(receipt_path: Path) -> dict[str, Any]:
    receipt = read_object(receipt_path)
    if receipt.get("schema_version") != 1:
        raise NamespaceError("receipt schema_version must be 1")
    evidence_root = receipt.get("evidence_root")
    token = receipt.get("token")
    if not isinstance(evidence_root, str) or not evidence_root:
        raise NamespaceError("receipt evidence_root must be a nonempty string")
    if not isinstance(token, str) or not token:
        raise NamespaceError("receipt token must be a nonempty string")
    root = Path(evidence_root)
    marker = root / MARKER_NAME
    entries = sorted(path.name for path in root.iterdir()) \
        if root.is_dir() else []
    stored = read_object(marker) if marker.is_file() else {}
    token_equal = stored.get("token") == token
    root_equal = stored.get("evidence_root") == str(root.resolve())
    ready = entries == [MARKER_NAME] and token_equal and root_equal
    return {
        "schema_version": 1,
        "ready": ready,
        "evidence_root": str(root.resolve()),
        "token_equal": token_equal,
        "root_equal": root_equal,
        "entries": entries,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)

    exact = commands.add_parser("reserve")
    exact.add_argument("path", type=Path)
    exact.add_argument("receipt", type=Path)
    exact.add_argument("--token")

    next_parser = commands.add_parser("reserve-next")
    next_parser.add_argument("parent", type=Path)
    next_parser.add_argument("stem")
    next_parser.add_argument("receipt", type=Path)
    next_parser.add_argument("--token")

    check = commands.add_parser("check")
    check.add_argument("receipt", type=Path)
    check.add_argument("output", type=Path)
    args = parser.parse_args()

    try:
        if args.command == "reserve":
            if is_within(args.receipt, args.path):
                raise NamespaceError(
                    "receipt must be outside the evidence namespace")
            result = reserve_exact(args.path, args.token)
            write_json(args.receipt, result)
        elif args.command == "reserve-next":
            result = reserve_next(args.parent, args.stem, args.token)
            root = Path(result["evidence_root"])
            if is_within(args.receipt, root):
                root.rmdir()
                raise NamespaceError(
                    "receipt must be outside the evidence namespace")
            write_json(args.receipt, result)
        else:
            result = validate_receipt(args.receipt)
            root = Path(result["evidence_root"])
            if is_within(args.output, root):
                raise NamespaceError(
                    "check output must be outside the evidence namespace")
            write_json(args.output, result)
        return 0 if result.get("ready", True) else 2
    except (OSError, json.JSONDecodeError, NamespaceError) as error:
        parser.error(str(error))
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
