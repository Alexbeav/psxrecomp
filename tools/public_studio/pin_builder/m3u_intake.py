#!/usr/bin/env python3
"""Fail-closed M3U and multi-disc CHD intake for PSX Workbench."""

from __future__ import annotations

import hashlib
import json
import os
import re
import time
from pathlib import Path, PurePosixPath
from typing import Any

try:
    from . import chd_intake
except ImportError:
    import chd_intake  # type: ignore


CONTRACT = "psx-workbench-m3u-intake-v1"
CONFIGURATION = {
    "playlist_encoding": "utf-8-sig-strict",
    "entry_type": ".chd",
    "disc_directory_pattern": "disc-{index:03d}",
    "boot_disc_index": 1,
}


class M3UIntakeError(RuntimeError):
    pass


def sha256(path: Path) -> str:
    return chd_intake.sha256(path)


def identity(path: Path) -> dict[str, Any]:
    return chd_intake.identity(path)


def _contract_identities() -> dict[str, Any]:
    implementation = Path(__file__).resolve()
    schema = implementation.parents[1] / "schema" / "m3u-intake-receipt.schema.json"
    if not schema.is_file():
        raise M3UIntakeError("the M3U receipt schema is absent")
    return {
        "implementation": identity(implementation),
        "schema": identity(schema),
        "configuration": {
            "value": CONFIGURATION,
            "sha256": hashlib.sha256(json.dumps(
                CONFIGURATION, sort_keys=True, separators=(",", ":")
            ).encode("utf-8")).hexdigest(),
        },
    }


def parse_playlist(playlist: Path) -> list[dict[str, Any]]:
    if not playlist.is_file() or playlist.suffix.lower() != ".m3u":
        raise M3UIntakeError("playlist must be an existing .m3u file")
    try:
        lines = playlist.read_text(encoding="utf-8-sig", errors="strict").splitlines()
    except UnicodeDecodeError as error:
        raise M3UIntakeError("playlist must be valid UTF-8 text") from error
    root = playlist.parent.resolve()
    entries: list[dict[str, Any]] = []
    seen: set[str] = set()
    for line_number, raw in enumerate(lines, 1):
        value = raw.strip()
        if not value or value.startswith("#"):
            continue
        normalized = value.replace("\\", "/")
        pure = PurePosixPath(normalized)
        if (pure.is_absolute() or re.match(r"^[A-Za-z]:", normalized) or
                any(part in ("", ".", "..") for part in pure.parts)):
            raise M3UIntakeError(f"playlist entry is not a safe relative path at line {line_number}")
        if pure.suffix.lower() != ".chd":
            raise M3UIntakeError(f"playlist entry is not a supported CHD at line {line_number}")
        key = pure.as_posix().casefold()
        if key in seen:
            raise M3UIntakeError(f"playlist contains a duplicate or ambiguous entry at line {line_number}")
        seen.add(key)
        source = (root / Path(*pure.parts)).resolve()
        try:
            source.relative_to(root)
        except ValueError as error:
            raise M3UIntakeError(f"playlist entry escapes its source directory at line {line_number}") from error
        chd_intake.validate_source(source)
        entries.append({"index": len(entries) + 1, "entry": pure.as_posix(),
                        "source": identity(source),
                        "subchannel": chd_intake.subchannel_companion(source)})
    if len(entries) < 2:
        raise M3UIntakeError("playlist must contain at least two unique CHD entries")
    return entries


def preview(playlist: Path, stage: Path, tool: Path) -> dict[str, Any]:
    entries = parse_playlist(playlist)
    tool_id = chd_intake.validate_tool(tool)
    return {
        "schema_version": 1,
        "contract": CONTRACT,
        "action": "extract-m3u-chd-set",
        "source_playlist": identity(playlist),
        "playlist_order": entries,
        "disc_count": len(entries),
        "stage": str(stage),
        "stage_must_be_new": True,
        "required_free_bytes_estimate": sum(
            entry["source"]["length"] for entry in entries) * 3,
        "tool": tool_id,
        "contract_identities": _contract_identities(),
        "promotion": False,
        "network_required": False,
        "disc_swap": {
            "playlist_order_authoritative": True,
            "boot_disc_index": 1,
            "runtime_capability": "not-qualified",
            "tested": False,
            "abstention": "intake does not prove runtime disc swapping",
        },
    }


def _boot_identity(cue: Path) -> dict[str, Any]:
    try:
        from . import baseline_workflow
    except ImportError:
        import baseline_workflow  # type: ignore
    return baseline_workflow.disc_boot_identity(cue)


def extract(playlist: Path, stage: Path, tool: Path) -> dict[str, Any]:
    plan = preview(playlist, stage, tool)
    if stage.exists():
        raise M3UIntakeError("staging output is occupied; intake never overwrites")
    stage.mkdir(parents=True)
    started = time.time()
    discs: list[dict[str, Any]] = []
    resolved_lines: list[str] = []
    try:
        for entry in plan["playlist_order"]:
            index = entry["index"]
            disc_stage = stage / f"disc-{index:03d}"
            source = Path(entry["source"]["path"])
            _require_identity(source, entry["source"], f"disc {index} source")
            receipt = chd_intake.extract(source, disc_stage, tool)
            if receipt["source"] != entry["source"] or not receipt.get("source_unchanged"):
                raise M3UIntakeError(f"disc {index} source changed during extraction")
            if receipt.get("subchannel", {}).get("status") == "preserved":
                if receipt["subchannel"].get("source") != entry["subchannel"].get("source"):
                    raise M3UIntakeError(f"disc {index} SBI companion changed during extraction")
                subchannel = receipt["subchannel"] | {
                    "output": receipt["subchannel"]["output"] | {
                        "path": (disc_stage / receipt["subchannel"]["output"]["path"]
                                 ).relative_to(stage).as_posix()}}
            else:
                subchannel = receipt.get("subchannel", entry["subchannel"])
            receipt_path = disc_stage / "receipt.json"
            cue = disc_stage / receipt["cue"]["path"]
            relative_cue = cue.relative_to(stage).as_posix()
            resolved_lines.append(relative_cue)
            tracks = []
            for track in receipt["track_files"]:
                tracks.append(track | {"path": (disc_stage / track["path"]).relative_to(stage).as_posix()})
            discs.append({
                "index": index,
                "source": entry["source"],
                "source_entry": entry["entry"],
                "stage": disc_stage.relative_to(stage).as_posix(),
                "chd_receipt": identity(receipt_path) | {"path": receipt_path.relative_to(stage).as_posix()},
                "cue": identity(cue) | {"path": relative_cue},
                "track_files": tracks,
                "subchannel": subchannel,
                "boot_executable": _boot_identity(cue),
            })
        resolved = stage / "resolved.m3u"
        resolved.write_text("\n".join(resolved_lines) + "\n", encoding="utf-8", newline="\n")
        resident = discs[0]["boot_executable"]
        receipt = {
            "schema_version": 1,
            "contract": CONTRACT,
            "result": "pass",
            "source_playlist": plan["source_playlist"],
            "source_unchanged": identity(playlist) == plan["source_playlist"],
            "playlist_order": plan["playlist_order"],
            "disc_count": len(discs),
            "tool": plan["tool"],
            "contract_identities": plan["contract_identities"],
            "resolved_playlist": identity(resolved) | {"path": resolved.name},
            "discs": discs,
            "resident_executable": resident | {
                "basis": "boot-disc executable identity; residency across disc swaps is untested"},
            "disc_swap": plan["disc_swap"],
            "elapsed_seconds": round(time.time() - started, 3),
            "promotion": False,
            "cleanup_allowed": False,
        }
        if not receipt["source_unchanged"]:
            raise M3UIntakeError("source playlist changed during extraction")
        temporary = stage / ".receipt.json.tmp"
        temporary.write_text(json.dumps(receipt, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        os.replace(temporary, stage / "receipt.json")
        return receipt
    except BaseException as error:
        failure = stage / "failure.json"
        if not failure.exists():
            failure.write_text(json.dumps({"schema_version": 1, "contract": CONTRACT,
                                           "result": "blocked", "error": str(error)},
                                          indent=2, sort_keys=True) + "\n", encoding="utf-8")
        raise


def validate_receipt(receipt_path: Path) -> dict[str, Any]:
    if not receipt_path.is_file():
        raise M3UIntakeError("the M3U intake receipt is absent")
    try:
        value = json.loads(receipt_path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise M3UIntakeError("the M3U intake receipt is not valid UTF-8 JSON") from error
    if not isinstance(value, dict):
        raise M3UIntakeError("the M3U intake receipt root is invalid")
    if (value.get("schema_version") != 1 or value.get("contract") != CONTRACT or
            value.get("result") != "pass"):
        raise M3UIntakeError("the M3U intake receipt is not a successful v1 receipt")
    root = receipt_path.parent.resolve()
    actual_contract = _contract_identities()
    for name in ("implementation", "schema"):
        expected_contract = value.get("contract_identities", {}).get(name, {})
        if (expected_contract.get("sha256") != actual_contract[name]["sha256"] or
                expected_contract.get("length") != actual_contract[name]["length"]):
            raise M3UIntakeError(f"the M3U {name} identity changed")
    if value.get("contract_identities", {}).get("configuration") != actual_contract["configuration"]:
        raise M3UIntakeError("the M3U configuration identity changed")
    tool_value = value.get("tool")
    if not isinstance(tool_value, dict) or not isinstance(tool_value.get("path"), str):
        raise M3UIntakeError("the M3U conversion tool identity is absent")
    tool_path = Path(tool_value["path"])
    actual_tool = chd_intake.validate_tool(tool_path)
    if (actual_tool.get("sha256") != tool_value.get("sha256") or
            actual_tool.get("version") != tool_value.get("version")):
        raise M3UIntakeError("the M3U conversion tool identity changed")
    discs = value.get("discs")
    order = value.get("playlist_order")
    if (not isinstance(discs, list) or not isinstance(order, list) or
            value.get("disc_count") != len(discs) or len(order) != len(discs) or
            len(discs) < 2):
        raise M3UIntakeError("the M3U receipt disc count is invalid")
    source_playlist = value.get("source_playlist")
    resolved_playlist = value.get("resolved_playlist")
    if (not isinstance(source_playlist, dict) or
            not isinstance(source_playlist.get("path"), str) or
            not isinstance(resolved_playlist, dict)):
        raise M3UIntakeError("the M3U playlist identity is absent")
    playlist_source = Path(source_playlist["path"])
    _require_identity(playlist_source, source_playlist, "source playlist")
    _require_relative_identity(root, resolved_playlist, "resolved playlist")
    seen: set[str] = set()
    resolved_lines: list[str] = []
    for expected_index, disc in enumerate(discs, 1):
        if not isinstance(disc, dict) or disc.get("index") != expected_index:
            raise M3UIntakeError("the M3U receipt disc order is invalid")
        disc_source = disc.get("source")
        if not isinstance(disc_source, dict) or not isinstance(disc_source.get("path"), str):
            raise M3UIntakeError(f"disc {expected_index} source identity is absent")
        source = Path(disc_source["path"])
        _require_identity(source, disc_source, f"disc {expected_index} source")
        key = str(source.resolve()).casefold()
        if key in seen:
            raise M3UIntakeError("the M3U receipt contains duplicate disc sources")
        seen.add(key)
        for key_name, label in (("chd_receipt", "CHD receipt"), ("cue", "CUE")):
            if not isinstance(disc.get(key_name), dict):
                raise M3UIntakeError(f"disc {expected_index} {label} identity is absent")
            _require_relative_identity(root, disc[key_name], f"disc {expected_index} {label}")
        resolved_lines.append(disc["cue"]["path"])
        tracks = disc.get("track_files")
        if not isinstance(tracks, list) or not tracks:
            raise M3UIntakeError(f"disc {expected_index} track identities are absent")
        for track in tracks:
            if not isinstance(track, dict):
                raise M3UIntakeError(f"disc {expected_index} track identity is invalid")
            _require_relative_identity(root, track, f"disc {expected_index} track")
        subchannel = disc.get("subchannel")
        if not isinstance(subchannel, dict) or subchannel.get("status") not in {"absent", "preserved"}:
            raise M3UIntakeError(f"disc {expected_index} subchannel status is invalid")
        if subchannel["status"] == "preserved":
            source_sbi = subchannel.get("source")
            output_sbi = subchannel.get("output")
            if (not isinstance(source_sbi, dict) or not isinstance(source_sbi.get("path"), str) or
                    not isinstance(output_sbi, dict)):
                raise M3UIntakeError(f"disc {expected_index} SBI identity is absent")
            _require_identity(Path(source_sbi["path"]), source_sbi,
                              f"disc {expected_index} SBI source")
            _require_relative_identity(root, output_sbi,
                                       f"disc {expected_index} SBI output")
            if any(source_sbi.get(key) != output_sbi.get(key)
                   for key in ("length", "sha256", "record_count")):
                raise M3UIntakeError(f"disc {expected_index} SBI copy identity changed")
        ordered = order[expected_index - 1]
        if not isinstance(ordered, dict):
            raise M3UIntakeError("the source playlist order is invalid")
        if ordered.get("index") != expected_index or ordered.get("entry") != disc.get("source_entry"):
            raise M3UIntakeError("the source playlist order does not match the disc evidence")
        if ordered.get("source", {}).get("sha256") != disc.get("source", {}).get("sha256"):
            raise M3UIntakeError("the source playlist identity does not match the disc evidence")
        cue_path = root / Path(*PurePosixPath(disc["cue"]["path"]).parts)
        actual_boot = _boot_identity(cue_path)
        recorded_boot = disc.get("boot_executable")
        if not isinstance(recorded_boot, dict) or any(
                recorded_boot.get(key) != actual_boot.get(key)
                for key in ("path", "serial", "length", "sha256")):
            raise M3UIntakeError(f"disc {expected_index} boot executable identity changed")
    resident = value.get("resident_executable")
    boot_disc = discs[0].get("boot_executable", {})
    if not isinstance(resident, dict) or any(
            resident.get(key) != boot_disc.get(key)
            for key in ("path", "serial", "length", "sha256")):
        raise M3UIntakeError("the resident executable identity does not match the boot disc")
    resolved_path = root / Path(*PurePosixPath(resolved_playlist["path"]).parts)
    actual_lines = [line.strip() for line in resolved_path.read_text(
        encoding="utf-8-sig", errors="strict").splitlines() if line.strip()]
    if actual_lines != resolved_lines:
        raise M3UIntakeError("the resolved playlist order changed")
    swap = value.get("disc_swap")
    if (not isinstance(swap, dict) or swap.get("playlist_order_authoritative") is not True or
            swap.get("boot_disc_index") != 1 or
            swap.get("runtime_capability") != "not-qualified" or
            swap.get("tested") is not False or not isinstance(swap.get("abstention"), str) or
            not swap["abstention"]):
        raise M3UIntakeError("the M3U receipt disc-swap abstention is invalid")
    return value


def _require_identity(path: Path, expected: dict[str, Any], label: str) -> None:
    if not path.is_file() or path.stat().st_size != expected.get("length") or sha256(path) != expected.get("sha256"):
        raise M3UIntakeError(f"{label} identity changed")


def _require_relative_identity(root: Path, expected: dict[str, Any], label: str) -> None:
    raw = expected.get("path")
    if not isinstance(raw, str):
        raise M3UIntakeError(f"{label} path is absent")
    pure = PurePosixPath(raw)
    if pure.is_absolute() or ".." in pure.parts:
        raise M3UIntakeError(f"{label} path is unsafe")
    path = (root / Path(*pure.parts)).resolve()
    try:
        path.relative_to(root)
    except ValueError as error:
        raise M3UIntakeError(f"{label} escapes the receipt root") from error
    _require_identity(path, expected, label)
