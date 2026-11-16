#!/usr/bin/env python3
"""Validate frozen Workbench experiment contracts before and during a run."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import socket
from pathlib import Path
from typing import Any, Callable


ROOT = Path(__file__).resolve().parents[2]
VARIABLE = re.compile(r"\$\{([A-Za-z_][A-Za-z0-9_]*)\}")


class ContractError(ValueError):
    pass


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def sha256_source_file(path: Path) -> str:
    """Hash source text with Git-style LF line endings."""
    data = path.read_bytes().replace(b"\r\n", b"\n")
    return hashlib.sha256(data).hexdigest()


def sha256_directory(path: Path) -> str:
    """Hash an exact directory tree, including relative names and file bytes."""
    digest = hashlib.sha256()
    files = sorted(item for item in path.rglob("*") if item.is_file())
    for item in files:
        if item.is_symlink():
            raise ContractError(f"directory artifact contains symlink: {item}")
        relative = item.relative_to(path).as_posix().encode("utf-8")
        digest.update(len(relative).to_bytes(4, "big"))
        digest.update(relative)
        size = item.stat().st_size
        digest.update(size.to_bytes(8, "big"))
        with item.open("rb") as source:
            for chunk in iter(lambda: source.read(1024 * 1024), b""):
                digest.update(chunk)
    return digest.hexdigest()


def route_sha256(route: dict[str, Any]) -> str:
    payload = {
        "start_frame": route["start_frame"],
        "target_relative_frame": route["target_relative_frame"],
        "steps": route["steps"],
    }
    encoded = json.dumps(payload, sort_keys=True, separators=(",", ":")).encode()
    return hashlib.sha256(encoded).hexdigest()


def parse_variables(values: list[str]) -> dict[str, str]:
    variables = {"PSX_REFERENCES": str(ROOT)}
    variables.update(os.environ)
    for value in values:
        key, separator, item = value.partition("=")
        if not separator or not key:
            raise ContractError(f"invalid variable {value!r}; expected NAME=VALUE")
        variables[key] = item
    return variables


def expand(value: Any, variables: dict[str, str]) -> Any:
    if isinstance(value, str):
        def replace(match: re.Match[str]) -> str:
            key = match.group(1)
            if key not in variables:
                raise ContractError(f"undefined contract variable: {key}")
            return variables[key]
        return VARIABLE.sub(replace, value)
    if isinstance(value, list):
        return [expand(item, variables) for item in value]
    if isinstance(value, dict):
        return {key: expand(item, variables) for key, item in value.items()}
    return value


def load_contract(path: Path, variables: dict[str, str]) -> dict[str, Any]:
    contract = expand(json.loads(path.read_text(encoding="utf-8")), variables)
    if contract.get("schema_version") != 1:
        raise ContractError("unsupported experiment-contract schema_version")
    return contract


def _require_hash(path: Path, expected: str, label: str) -> None:
    actual = sha256_file(path)
    if actual.lower() != expected.lower():
        raise ContractError(f"{label} hash mismatch: expected {expected}, got {actual}")


def static_preflight(contract: dict[str, Any]) -> dict[str, Any]:
    checked: list[str] = []
    for artifact in contract.get("artifacts", []):
        path = Path(artifact["path"])
        kind = artifact.get("kind", "file")
        exists = path.is_file() if kind == "file" else path.is_dir()
        if not exists:
            raise ContractError(f"missing {artifact['id']} ({kind}): {path}")
        if artifact.get("sha256"):
            if kind == "file":
                _require_hash(path, artifact["sha256"], artifact["id"])
            else:
                actual = sha256_directory(path)
                if actual.lower() != artifact["sha256"].lower():
                    raise ContractError(
                        f"{artifact['id']} tree hash mismatch: expected "
                        f"{artifact['sha256']}, got {actual}")
        checked.append(artifact["id"])

    route = contract["route"]
    driver = Path(route["driver"])
    if not driver.is_file():
        raise ContractError(f"route driver missing: {driver}")
    actual_driver_hash = sha256_source_file(driver)
    if actual_driver_hash.lower() != route["driver_sha256"].lower():
        raise ContractError(
            "route driver hash mismatch: expected "
            f"{route['driver_sha256']}, got {actual_driver_hash}"
        )
    actual_route_hash = route_sha256(route)
    if actual_route_hash.lower() != route["sha256"].lower():
        raise ContractError(
            f"route hash mismatch: expected {route['sha256']}, got {actual_route_hash}"
        )
    if sum(step["frames"] for step in route["steps"]) - 1 != route["target_relative_frame"]:
        raise ContractError("route duration does not equal target_relative_frame + 1")

    cache = contract.get("cache")
    cache_summary = None
    if cache:
        dll = Path(cache["dll"])
        ranges = Path(cache["ranges"])
        for path, key in ((dll, "dll_sha256"), (ranges, "ranges_sha256")):
            if not path.is_file():
                raise ContractError(f"cache artifact missing: {path}")
            _require_hash(path, cache[key], path.name)
        entries = sum(1 for line in ranges.read_text(encoding="utf-8").splitlines()
                      if line.startswith("F "))
        if entries != cache["expected_manifest_entries"]:
            raise ContractError(
                f"cache manifest entries: expected {cache['expected_manifest_entries']}, got {entries}"
            )
        cache_summary = {"manifest_entries": entries, "dll": str(dll)}

    for value in contract.get("isolation", {}).get("must_not_exist", []):
        path = Path(value)
        if path.exists():
            raise ContractError(f"isolation path is not fresh: {path}")

    return {
        "ok": True,
        "experiment_id": contract["experiment_id"],
        "artifacts_checked": checked,
        "route_sha256": actual_route_hash,
        "route_steps": len(route["steps"]),
        "cache": cache_summary,
    }


def _lookup(response: dict[str, Any], dotted: str) -> Any:
    value: Any = response
    for part in dotted.split("."):
        if not isinstance(value, dict) or part not in value:
            raise ContractError(f"live response lacks {dotted!r}")
        value = value[part]
    return value


def _assert_value(actual: Any, assertion: dict[str, Any], label: str) -> None:
    if "equals" in assertion and actual != assertion["equals"]:
        raise ContractError(f"{label}: expected {assertion['equals']!r}, got {actual!r}")
    if "at_least" in assertion and actual < assertion["at_least"]:
        raise ContractError(f"{label}: expected >= {assertion['at_least']!r}, got {actual!r}")


def live_preflight(
    contract: dict[str, Any],
    phase: str,
    query: Callable[[dict[str, Any]], dict[str, Any]],
) -> dict[str, Any]:
    results = []
    checks = [item for item in contract.get("live_preflight", []) if item["phase"] == phase]
    if not checks:
        raise ContractError(f"contract has no live_preflight checks for phase {phase!r}")
    for check in checks:
        response = query(check["command"])
        if not response.get("ok"):
            raise ContractError(f"{check['id']}: command failed: {response}")
        for assertion in check.get("assertions", []):
            actual = _lookup(response, assertion["path"])
            _assert_value(actual, assertion, f"{check['id']}.{assertion['path']}")
        results.append({"id": check["id"], "ok": True})
    return {"ok": True, "phase": phase, "checks": results}


def socket_query(port: int, request: dict[str, Any]) -> dict[str, Any]:
    payload = (json.dumps(request, separators=(",", ":")) + "\n").encode()
    with socket.create_connection(("127.0.0.1", port), timeout=10) as connection:
        connection.sendall(payload)
        data = connection.makefile("rb").readline()
    if not data:
        raise ContractError("debug server returned no response")
    return json.loads(data)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("contract", type=Path)
    parser.add_argument("--var", action="append", default=[], metavar="NAME=VALUE")
    parser.add_argument("--live-phase")
    parser.add_argument("--port", type=int)
    args = parser.parse_args()
    try:
        contract = load_contract(args.contract, parse_variables(args.var))
        if args.live_phase:
            if args.port is None:
                parser.error("--port is required with --live-phase")
            result = live_preflight(
                contract, args.live_phase, lambda request: socket_query(args.port, request)
            )
        else:
            result = static_preflight(contract)
    except (ContractError, OSError, json.JSONDecodeError) as exc:
        print(json.dumps({"ok": False, "error": str(exc)}))
        return 1
    print(json.dumps(result, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
