#!/usr/bin/env python3
"""Compare two generated PSXRecomp projects with narrow path normalization."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re


def project_files(root: Path, excluded_top_levels: set[str]) -> dict[str, Path]:
    files: dict[str, Path] = {}
    for path in root.rglob("*"):
        if not path.is_file():
            continue
        relative = path.relative_to(root)
        if relative.parts[0] in excluded_top_levels:
            continue
        files[relative.as_posix()] = path
    return files


def normalized_bytes(path: Path, root: Path) -> bytes:
    data = path.read_bytes()
    try:
        text = data.decode("utf-8")
    except UnicodeDecodeError:
        return data

    resolved = str(root.resolve())
    variants = {
        resolved,
        resolved.replace("\\", "/"),
        resolved.replace("/", "\\"),
    }
    for variant in sorted(variants, key=len, reverse=True):
        text = text.replace(variant, "<PROJECT_ROOT>")
    return text.encode("utf-8")


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


BUILD_ID_TIMESTAMP = re.compile(
    rb"\([A-Z][a-z]{2} [ 0-9][0-9] [0-9]{4} "
    rb"[0-9]{2}:[0-9]{2}:[0-9]{2}\)")


def normalized_pe_product(data: bytes) -> tuple[bytes, dict[str, object]]:
    if len(data) < 0x40 or data[:2] != b"MZ":
        raise ValueError("build product is not a PE executable")
    pe_offset = int.from_bytes(data[0x3C:0x40], "little")
    if pe_offset + 92 > len(data) or data[pe_offset:pe_offset + 4] != b"PE\0\0":
        raise ValueError("build product has an invalid PE header")

    normalized = bytearray(data)
    timestamp_offset = pe_offset + 8
    checksum_offset = pe_offset + 88
    timestamp = int.from_bytes(
        normalized[timestamp_offset:timestamp_offset + 4], "little")
    checksum = int.from_bytes(
        normalized[checksum_offset:checksum_offset + 4], "little")
    normalized[timestamp_offset:timestamp_offset + 4] = b"\0" * 4
    normalized[checksum_offset:checksum_offset + 4] = b"\0" * 4
    normalized_bytes_value, build_id_count = BUILD_ID_TIMESTAMP.subn(
        b"(<BUILD_TIMESTAMP>)", bytes(normalized))
    return normalized_bytes_value, {
        "pe_timestamp": timestamp,
        "pe_checksum": f"0x{checksum:08x}",
        "build_id_timestamps_normalized": build_id_count,
    }


def compare_pe_product(root_a: Path, root_b: Path, relative: Path) -> dict[str, object]:
    path_a = root_a / relative
    path_b = root_b / relative
    data_a = path_a.read_bytes()
    data_b = path_b.read_bytes()
    normalized_a, metadata_a = normalized_pe_product(data_a)
    normalized_b, metadata_b = normalized_pe_product(data_b)
    different_bytes = sum(a != b for a, b in zip(data_a, data_b))
    different_bytes += abs(len(data_a) - len(data_b))
    return {
        "product": relative.as_posix(),
        "size_a": len(data_a),
        "size_b": len(data_b),
        "sha256_a": digest(data_a),
        "sha256_b": digest(data_b),
        "different_bytes": different_bytes,
        "metadata_a": metadata_a,
        "metadata_b": metadata_b,
        "normalized_sha256_a": digest(normalized_a),
        "normalized_sha256_b": digest(normalized_b),
        "reproducible": normalized_a == normalized_b,
    }


def compare(
        root_a: Path, root_b: Path,
        excluded_top_levels: set[str]) -> dict[str, object]:
    files_a = project_files(root_a, excluded_top_levels)
    files_b = project_files(root_b, excluded_top_levels)
    names_a = set(files_a)
    names_b = set(files_b)
    only_a = sorted(names_a - names_b)
    only_b = sorted(names_b - names_a)
    differing: list[dict[str, object]] = []
    entries: list[tuple[str, str, int]] = []

    for relative in sorted(names_a & names_b):
        data_a = normalized_bytes(files_a[relative], root_a)
        data_b = normalized_bytes(files_b[relative], root_b)
        hash_a = digest(data_a)
        hash_b = digest(data_b)
        if hash_a != hash_b:
            differing.append({
                "path": relative,
                "size_a": len(data_a),
                "size_b": len(data_b),
                "sha256_a": hash_a,
                "sha256_b": hash_b,
            })
        else:
            entries.append((relative, hash_a, len(data_a)))

    tree_hasher = hashlib.sha256()
    for relative, file_hash, size in entries:
        tree_hasher.update(relative.encode("utf-8"))
        tree_hasher.update(b"\0")
        tree_hasher.update(file_hash.encode("ascii"))
        tree_hasher.update(b"\0")
        tree_hasher.update(str(size).encode("ascii"))
        tree_hasher.update(b"\n")

    return {
        "root_a": str(root_a.resolve()),
        "root_b": str(root_b.resolve()),
        "excluded_top_levels": sorted(excluded_top_levels),
        "files_a": len(files_a),
        "files_b": len(files_b),
        "only_a": only_a,
        "only_b": only_b,
        "differing": differing,
        "identical_files": len(entries),
        "identical_tree_sha256": tree_hasher.hexdigest(),
        "reproducible": not only_a and not only_b and not differing,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("root_a", type=Path)
    parser.add_argument("root_b", type=Path)
    parser.add_argument(
        "--include-build", action="store_true",
        help="include the top-level build directory (excluded by default)")
    parser.add_argument(
        "--exclude-top-level", action="append", default=[], metavar="NAME",
        help="exclude an additional top-level directory (repeatable)")
    parser.add_argument(
        "--max-differences", type=int, default=20,
        help="maximum differences printed per category (default: 20)")
    parser.add_argument(
        "--pe-product", type=Path,
        help="compare one PE build product after normalizing only its PE "
             "timestamp, checksum, and __DATE__/__TIME__ build ID")
    args = parser.parse_args()

    for root in (args.root_a, args.root_b):
        if not root.is_dir():
            parser.error(f"project root is not a directory: {root}")

    if args.pe_product:
        try:
            result = compare_pe_product(
                args.root_a, args.root_b, args.pe_product)
        except (OSError, ValueError) as exc:
            parser.error(str(exc))
        print(json.dumps(result, indent=2, sort_keys=True))
        return 0 if result["reproducible"] else 1

    excluded_top_levels = set(args.exclude_top_level)
    if not args.include_build:
        excluded_top_levels.add("build")
    result = compare(args.root_a, args.root_b, excluded_top_levels)
    limit = max(args.max_differences, 0)
    display = dict(result)
    display["only_a"] = result["only_a"][:limit]
    display["only_b"] = result["only_b"][:limit]
    display["differing"] = result["differing"][:limit]
    print(json.dumps(display, indent=2, sort_keys=True))
    return 0 if result["reproducible"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
