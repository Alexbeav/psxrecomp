#!/usr/bin/env python3
"""Normalize static entry candidates without promoting them to functions."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import re
import struct
from collections import defaultdict
from pathlib import Path
from typing import Any


ADDRESS_LINE = re.compile(r"^(0x[0-9a-fA-F]{8})\s+\S+")
SIZE_LINE = re.compile(r"(\d+) bytes")


def address(value: int) -> str:
    return f"0x{value:08X}"


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def candidate_id(content_sha256: str, load_address: int, entry: int) -> str:
    key = f"{content_sha256.lower()}:{load_address:08X}:{entry:08X}".encode()
    return "ce_" + hashlib.sha256(key).hexdigest()[:16]


def load_source(path: Path, source_format: str) -> dict[int, int | None]:
    if source_format == "recompone_json":
        data = json.loads(path.read_text(encoding="utf-8"))
        return {
            int(row["address"], 16): int(row["size"])
            for row in data.get("functions", [])
        }
    if source_format == "recompsx_text":
        lines = path.read_text(encoding="utf-8-sig").splitlines()
        result: dict[int, int | None] = {}
        for index, line in enumerate(lines):
            match = ADDRESS_LINE.match(line)
            if not match:
                continue
            size = None
            if index + 1 < len(lines):
                size_match = SIZE_LINE.search(lines[index + 1])
                if size_match:
                    size = int(size_match.group(1))
            result[int(match.group(1), 16)] = size
        return result
    if source_format == "address_list":
        result = {}
        for line in path.read_text(encoding="utf-8-sig").splitlines():
            value = line.strip().split("#", 1)[0].strip()
            if value:
                result[int(value, 0)] = None
        return result
    if source_format == "ghidra_csv":
        result = {}
        with path.open(encoding="utf-8-sig", newline="") as stream:
            for row in csv.DictReader(stream):
                start = row.get("entry_address") or row.get("address") or row.get("entry")
                if not start:
                    continue
                # Ghidra's function-body address count is not a contiguous extent.
                # Accept an extent only when an exporter names it explicitly.
                size = row.get("extent_bytes")
                result[int(start, 0)] = int(size, 0) if size else None
        return result
    if source_format == "candidate_json":
        data = json.loads(path.read_text(encoding="utf-8"))
        rows = data.get("candidates", data)
        result = {}
        for row in rows:
            extent = row.get("extent")
            result[int(row["entry_address"], 16)] = extent.get("bytes") if extent else None
        return result
    if source_format == "decomp_jsonl":
        result = {}
        with path.open(encoding="utf-8") as stream:
            for line in stream:
                if not line.strip():
                    continue
                row = json.loads(line)
                start = row.get("entry_address") or row.get("address")
                if not start:
                    continue
                size = row.get("size_bytes") or row.get("size")
                result[int(start, 0)] = int(size) if size else None
        return result
    if source_format == "runtime_json":
        data = json.loads(path.read_text(encoding="utf-8"))
        items = data if isinstance(data, list) else [data]
        if isinstance(data, dict) and isinstance(data.get("captures"), list):
            items.extend(data["captures"])
        result = {}
        for item in items:
            if not isinstance(item, dict):
                continue
            direct = item.get("entry_address")
            if direct:
                result[int(direct, 0)] = None
            for field in ("dispatch_entry_pcs", "function_entry_pcs", "entries"):
                for value in item.get(field, []):
                    result[int(value, 0) if isinstance(value, str) else int(value)] = None
        return result
    raise ValueError(f"unsupported source format: {source_format}")


def load_runtime_trace(path: Path) -> dict[int, dict[str, Any]]:
    """Load observed addresses while retaining frame and observation kind."""
    observed: dict[int, dict[str, Any]] = {}
    with path.open(encoding="utf-8") as stream:
        for line_number, line in enumerate(stream, 1):
            if not line.strip():
                continue
            if line.startswith("PSXWB_ORACLE\t"):
                line = line[len("PSXWB_ORACLE\t"):]
            row = json.loads(line)
            if not isinstance(row, dict) or row.get("record") != "checkpoint":
                continue
            frame = row.get("relative_frame")
            if not isinstance(frame, int) or frame < 0:
                raise ValueError(f"runtime trace line {line_number} has an invalid frame")
            trace = row.get("trace")
            if not isinstance(trace, dict):
                raise ValueError(f"runtime trace line {line_number} has no trace object")
            for field in (
                "executed_addresses", "function_candidates",
                "dispatch_candidates", "indirect_targets",
            ):
                values = trace.get(field)
                if not isinstance(values, list):
                    raise ValueError(f"runtime trace line {line_number}.{field} must be an array")
                for value in values:
                    entry = int(value, 0) if isinstance(value, str) else int(value)
                    current = observed.setdefault(entry, {"frames": set(), "kinds": set()})
                    current["frames"].add(frame)
                    current["kinds"].add(field)
    return observed


def decode_static_support(data: bytes, load: int, entries: set[int]) -> dict[int, set[str]]:
    """Classify evidence. This function does not discover new entries."""
    support: dict[int, set[str]] = defaultdict(set)
    words = list(struct.unpack(f"<{len(data) // 4}I", data[: len(data) // 4 * 4]))
    word_set = set(words)
    for entry in entries:
        offset = entry - load
        if offset % 4 or offset < 0 or offset + 4 > len(data):
            continue
        word = words[offset // 4]
        op = word >> 26
        rs = (word >> 21) & 31
        rt = (word >> 16) & 31
        imm = word & 0xFFFF
        if op == 9 and rs == 29 and rt == 29 and imm & 0x8000:
            support[entry].add("stack_frame_prologue")
        if entry in word_set:
            support[entry].add("aligned_address_word")

    for index, word in enumerate(words):
        pc = load + index * 4
        if word >> 26 == 3:
            target = ((pc + 4) & 0xF0000000) | ((word & 0x03FFFFFF) << 2)
            if target in entries:
                support[target].add("direct_jal_target")
        if word >> 26 != 15:
            continue
        register = (word >> 16) & 31
        high = (word & 0xFFFF) << 16
        for later in words[index + 1 : index + 6]:
            op = later >> 26
            rs = (later >> 21) & 31
            rt = (later >> 16) & 31
            if rs != register or rt != register or op not in (9, 13):
                continue
            low = later & 0xFFFF
            value = high | low if op == 13 else (high + struct.unpack("<h", struct.pack("<H", low))[0]) & 0xFFFFFFFF
            if value in entries:
                support[value].add("materialized_address")
    return support


def normalize(manifest_path: Path) -> dict[str, Any]:
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    modules = []
    for module in manifest["modules"]:
        binary_path = (manifest_path.parent / module["binary_path"]).resolve()
        raw = binary_path.read_bytes()
        binary_source_hash = hashlib.sha256(raw).hexdigest()
        offset = int(module.get("file_text_offset", 0))
        length = int(module.get("text_bytes", len(raw) - offset))
        data = raw[offset : offset + length]
        content_hash = hashlib.sha256(data).hexdigest()
        expected = module.get("content_sha256")
        if expected and content_hash != expected.lower():
            raise ValueError(f"{module['module_id']}: content hash mismatch")
        load = int(module["load_address"], 0)
        collected: dict[int, list[dict[str, Any]]] = defaultdict(list)
        for source in module["sources"]:
            source_path = (manifest_path.parent / source["path"]).resolve()
            source_artifact_hash = sha256_file(source_path)
            if source["format"] == "runtime_trace_jsonl":
                required = ("producer", "route_id", "backend_receipt_sha256")
                missing = [name for name in required if not source.get(name)]
                if missing:
                    raise ValueError(
                        "runtime_trace_jsonl source lacks " + ", ".join(missing))
                backend_receipt = str(source["backend_receipt_sha256"]).lower()
                if not re.fullmatch(r"[0-9a-f]{64}", backend_receipt):
                    raise ValueError("runtime trace backend receipt must be a SHA-256 digest")
                for entry, trace in load_runtime_trace(source_path).items():
                    if not (load <= entry < load + len(data)):
                        continue
                    frames = sorted(trace["frames"])
                    collected[entry].append({
                        "source": source["tool"],
                        "independence_group": source.get(
                            "independence_group", source["tool"]),
                        "evidence_class": "runtime_trace",
                        "relation": "supports",
                        "artifact_sha256": source_artifact_hash,
                        "producer": source["producer"],
                        "route_id": source["route_id"],
                        "frame_range": {"first": frames[0], "last": frames[-1]},
                        "observation_kinds": sorted(trace["kinds"]),
                        "backend": {
                            "id": source.get("backend_id", "unknown"),
                            "receipt_sha256": backend_receipt,
                        },
                    })
                continue
            for entry, extent in load_source(source_path, source["format"]).items():
                if not (load <= entry < load + len(data)):
                    continue
                evidence = {
                    "source": source["tool"],
                    "independence_group": source.get(
                        "independence_group", source["tool"]
                    ),
                    "evidence_class": source["evidence_class"],
                    "relation": source.get("relation", "proposes"),
                    "artifact_sha256": source_artifact_hash,
                }
                if source.get("tool_revision"):
                    evidence["tool_revision"] = source["tool_revision"]
                if extent:
                    evidence["extent_bytes"] = extent
                collected[entry].append(evidence)

        support = decode_static_support(data, load, set(collected))
        candidates = []
        for entry in sorted(collected):
            evidence = collected[entry]
            for evidence_class in sorted(support[entry]):
                evidence.append({
                    "source": "workbench-mips-evidence",
                    "evidence_class": evidence_class,
                    "relation": "supports",
                })
            tools = sorted({
                item["source"] for item in evidence
                if item["relation"] in {"proposes", "confirms"}
            })
            evidence_groups = sorted({
                item.get("independence_group", item["source"])
                for item in evidence
                if item["relation"] in {"proposes", "confirms"}
            })
            extent_evidence = [item for item in evidence if item.get("extent_bytes")]
            extents = [item["extent_bytes"] for item in extent_evidence]
            extent = None
            if extents and len(set(extents)) == 1:
                size = extents[0]
                extent_sources = {item["source"] for item in extent_evidence}
                method = "tool_reported_consensus" if len(extent_sources) >= 2 else "single_tool_reported"
                extent = {"end_address": address(entry + size), "bytes": size, "method": method}
            candidates.append({
                "schema_version": 1,
                "candidate_id": candidate_id(content_hash, load, entry),
                "project_id": manifest["project_id"],
                "module_id": module["module_id"],
                "content_identity": {
                    "sha256": content_hash,
                    "source_sha256": binary_source_hash,
                    "bytes": len(data),
                    "load_address": address(load),
                    "file_text_offset": offset,
                    "format": module["format"],
                    "logical_source": module["logical_source"],
                },
                "entry_address": address(entry),
                "extent": extent,
                "evidence": evidence,
                "tool_agreement": {
                    "independent_tools": len(tools),
                    "tools": tools,
                    "independent_evidence_groups": len(evidence_groups),
                    "evidence_groups": evidence_groups,
                },
                "boundary_status": "candidate",
                "owner_risk": "unknown",
                "annotation_safety": "review_only",
                "semantic_identity": {"status": "unknown"},
            })
            if any(item["evidence_class"] == "runtime_trace" for item in evidence):
                normal_evidence = any(
                    item["evidence_class"] != "runtime_trace"
                    and item["relation"] in {"proposes", "confirms"}
                    for item in evidence
                )
                candidates[-1]["runtime_trace"] = {
                    "present": True,
                    "only": not normal_evidence,
                    "admission_status": (
                        "normal_evidence_present" if normal_evidence
                        else "requires_normal_admission_evidence"
                    ),
                    "native_entry": False,
                    "function_identity": False,
                }
        modules.append({"module_id": module["module_id"], "candidates": candidates})
    return {
        "schema_version": 1,
        "project_id": manifest["project_id"],
        "methodology": "discovery_only_frozen_before_reference_reveal",
        "modules": modules,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("manifest", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    result = normalize(args.manifest)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
