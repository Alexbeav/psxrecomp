#!/usr/bin/env python3
"""Fail-closed, offline CHD-to-BIN/CUE staging for PSX Workbench."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import time
from pathlib import Path
from typing import Any

CHDMAN_VERSION = "0.280 (mame0280)"
CHDMAN_SHA256 = "bec6137fa912d9ac54de8549103867181c7ab6740a9004a8321941053b3b0e7f"
CHDMAN_LICENSE = "GPL-2.0-or-later (MAME package); chdman source identifies BSD-3-Clause"
CHDMAN_PROVENANCE = "https://github.com/mamedev/mame/releases/tag/mame0280"
CHD_MAGIC = b"MComprHD"
SBI_MAGIC = b"SBI\0"
SBI_RECORD_LENGTH = 14
FILE_RE = re.compile(r'^\s*FILE\s+"([^"]+)"\s+BINARY\s*$', re.I)
TRACK_RE = re.compile(r"^\s*TRACK\s+(\d+)\s+([^\s]+)\s*$", re.I)


class IntakeError(RuntimeError):
    pass


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def identity(path: Path) -> dict[str, Any]:
    return {"path": str(path), "length": path.stat().st_size, "sha256": sha256(path)}


def validate_source(source: Path) -> None:
    if not source.is_file() or source.suffix.lower() != ".chd":
        raise IntakeError("source must be an existing .chd file")
    with source.open("rb") as stream:
        if stream.read(8) != CHD_MAGIC:
            raise IntakeError("source extension is .chd but CHD magic is absent")


def _valid_bcd(value: int, maximum: int) -> bool:
    return (value & 0x0F) <= 9 and ((value >> 4) & 0x0F) <= 9 and \
        ((value >> 4) * 10 + (value & 0x0F)) <= maximum


def validate_sbi(path: Path) -> dict[str, Any]:
    data = path.read_bytes()
    if (len(data) < 4 + SBI_RECORD_LENGTH or data[:4] != SBI_MAGIC or
            (len(data) - 4) % SBI_RECORD_LENGTH):
        raise IntakeError("SBI companion has an invalid header or record length")
    positions: set[bytes] = set()
    for offset in range(4, len(data), SBI_RECORD_LENGTH):
        minute, second, frame, record_type = data[offset:offset + 4]
        position = data[offset:offset + 3]
        if (not _valid_bcd(minute, 99) or not _valid_bcd(second, 59) or
                not _valid_bcd(frame, 74) or record_type != 1):
            raise IntakeError(f"SBI companion has an invalid record at offset {offset}")
        if position in positions:
            raise IntakeError(f"SBI companion repeats a position at offset {offset}")
        positions.add(position)
    return identity(path) | {"record_count": len(positions)}


def subchannel_companion(source: Path) -> dict[str, Any]:
    path = source.with_suffix(".sbi")
    if not path.exists():
        return {"status": "absent", "qualified": False,
                "reason": "same-basename SBI companion is absent"}
    if not path.is_file():
        raise IntakeError("same-basename SBI companion is not a file")
    return {"status": "present", "qualified": True,
            "source": validate_sbi(path)}


def validate_tool(tool: Path, expected_hash: str = CHDMAN_SHA256,
                  expected_version: str = CHDMAN_VERSION) -> dict[str, Any]:
    if not tool.is_file() or sha256(tool) != expected_hash.lower():
        raise IntakeError("chdman executable hash is not the pinned hash")
    result = subprocess.run([str(tool)], capture_output=True, text=True, timeout=15)
    banner = result.stdout + result.stderr
    if expected_version not in banner:
        raise IntakeError("chdman version banner is not the pinned revision")
    value = identity(tool)
    value.update({"version": expected_version, "license": CHDMAN_LICENSE,
                  "provenance": CHDMAN_PROVENANCE})
    return value


def validate_cue(cue: Path) -> list[dict[str, Any]]:
    if not cue.is_file():
        raise IntakeError("chdman did not produce a CUE file")
    root = cue.parent.resolve()
    current: Path | None = None
    files: dict[Path, dict[str, Any]] = {}
    tracks: list[dict[str, Any]] = []
    for line in cue.read_text(encoding="utf-8-sig").splitlines():
        match = FILE_RE.match(line)
        if match:
            candidate = (root / match.group(1)).resolve()
            if candidate.parent != root:
                raise IntakeError("CUE track escapes the staging directory")
            current = candidate
            files.setdefault(candidate, {})
            continue
        match = TRACK_RE.match(line)
        if match:
            if current is None:
                raise IntakeError("CUE TRACK precedes FILE")
            tracks.append({"number": int(match.group(1)), "mode": match.group(2).upper(),
                           "file": current.name})
    if not tracks or [t["number"] for t in tracks] != list(range(1, len(tracks) + 1)):
        raise IntakeError("CUE tracks must be present and sequential from 1")
    if not any(t["mode"].startswith("MODE") for t in tracks):
        raise IntakeError("CUE has no data track")
    output = []
    for path in files:
        if not path.is_file() or path.stat().st_size == 0:
            raise IntakeError(f"CUE track body is absent or empty: {path.name}")
        modes = {t["mode"] for t in tracks if t["file"] == path.name}
        sectors = {2352 if mode.endswith("/2352") or mode == "AUDIO" else 2048 for mode in modes}
        if sectors and not any(path.stat().st_size % sector == 0 for sector in sectors):
            raise IntakeError(f"CUE track body is not sector aligned: {path.name}")
        output.append(identity(path) | {"path": path.name})
    return output


def preview(source: Path, stage: Path, tool: Path) -> dict[str, Any]:
    validate_source(source)
    tool_id = validate_tool(tool)
    return {"schema": 1, "action": "extract-chd", "source": identity(source),
            "subchannel": subchannel_companion(source),
            "stage": str(stage), "stage_must_be_new": True,
            "required_free_bytes_estimate": source.stat().st_size * 3,
            "tool": tool_id, "promotion": False, "network_required": False}


def extract(source: Path, stage: Path, tool: Path, runner=None) -> dict[str, Any]:
    plan = preview(source, stage, tool)
    if stage.exists():
        raise IntakeError("staging output is occupied; intake never overwrites")
    stage.mkdir(parents=True)
    cue = stage / f"{source.stem}.cue"
    body = stage / f"{source.stem}.bin"
    log = stage / "chdman.log"
    started = time.time()
    run = runner or (lambda argv, name: subprocess.run(argv, capture_output=True, text=True))
    result = run([str(tool), "verify", "-i", str(source)], "verify")
    log.write_text(result.stdout + result.stderr, encoding="utf-8")
    if result.returncode:
        raise IntakeError(f"chdman verify failed; staging preserved at {stage}")
    result = run([str(tool), "extractcd", "-i", str(source), "-o", str(cue),
                  "-ob", str(body)], "extract")
    with log.open("a", encoding="utf-8") as stream:
        stream.write(result.stdout + result.stderr)
    if result.returncode:
        raise IntakeError(f"chdman extractcd failed; staging preserved at {stage}")
    tracks = validate_cue(cue)
    subchannel = plan["subchannel"]
    if subchannel["status"] == "present":
        source_sbi = Path(subchannel["source"]["path"])
        if validate_sbi(source_sbi) != subchannel["source"]:
            raise IntakeError("SBI companion changed during extraction")
        staged_sbi = cue.with_suffix(".sbi")
        shutil.copy2(source_sbi, staged_sbi)
        staged_identity = validate_sbi(staged_sbi)
        subchannel = {"status": "preserved", "qualified": True,
                      "source": subchannel["source"],
                      "output": staged_identity | {"path": staged_sbi.name}}
    receipt = {"schema": 1, "result": "pass", "source": plan["source"],
               "source_unchanged": identity(source) == plan["source"], "tool": plan["tool"],
               "cue": identity(cue) | {"path": cue.name}, "track_files": tracks,
               "subchannel": subchannel,
               "elapsed_seconds": round(time.time() - started, 3), "promotion": False,
               "cleanup_allowed": False}
    if not receipt["source_unchanged"]:
        raise IntakeError("source changed during extraction")
    temporary = stage / ".receipt.json.tmp"
    temporary.write_text(json.dumps(receipt, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    os.replace(temporary, stage / "receipt.json")
    return receipt


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    for name in ("preview", "extract"):
        command = sub.add_parser(name)
        command.add_argument("source", type=Path)
        command.add_argument("stage", type=Path)
        command.add_argument("--chdman", type=Path, required=True)
    args = parser.parse_args()
    try:
        value = preview(args.source.resolve(), args.stage.resolve(), args.chdman.resolve()) \
            if args.command == "preview" else extract(args.source.resolve(), args.stage.resolve(), args.chdman.resolve())
    except IntakeError as error:
        print(json.dumps({"schema": 1, "result": "blocked", "error": str(error)}))
        return 2
    print(json.dumps(value, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
