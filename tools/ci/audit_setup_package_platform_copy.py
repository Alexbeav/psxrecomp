#!/usr/bin/env python3
"""Reject setup ZIPs whose toolchain copy does not match the host platform.

A Linux .tar.gz package is audited the same way, and its setup host must also
be mode 0755 (the reason a tarball ships beside the zip)."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
import tarfile
import zipfile
from pathlib import Path, PurePosixPath


PLATFORM_MARKERS = {
    "windows": ("windows-x64",),
    "linux": ("linux-x64",),
    "macos": ("macos-x64", "macos-arm64"),
}


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest().upper()


def infer_platform(path: Path) -> str:
    lower = path.name.lower()
    matches = [
        platform
        for platform, markers in PLATFORM_MARKERS.items()
        if any(marker in lower for marker in markers)
    ]
    if len(matches) != 1:
        raise ValueError(
            "archive name must contain exactly one supported platform marker: "
            "windows-x64, linux-x64, macos-x64, or macos-arm64"
        )
    return matches[0]


def root_member(names: list[str], basename: str) -> str:
    matches = [
        name
        for name in names
        if not name.endswith("/")
        and len(PurePosixPath(name).parts) == 1
        and PurePosixPath(name).name.lower() == basename.lower()
    ]
    if len(matches) != 1:
        raise ValueError(f"archive must contain exactly one root {basename}")
    return matches[0]


def require(text: str, tokens: tuple[str, ...], where: str) -> None:
    missing = [token for token in tokens if token not in text]
    if missing:
        raise ValueError(f"{where} is missing: {', '.join(missing)}")


def reject(text: str, tokens: tuple[str, ...], where: str) -> None:
    found = [token for token in tokens if token in text]
    if found:
        raise ValueError(f"{where} contains forbidden copy: {', '.join(found)}")


def is_tarball(path: Path) -> bool:
    return path.name.lower().endswith((".tar.gz", ".tgz"))


def read_members(path: Path) -> tuple[list[str], dict[str, int], object]:
    """Names, POSIX modes (tarball only) and a reader for one member."""
    if is_tarball(path):
        archive = tarfile.open(path, "r:gz")
        files = {m.name: m for m in archive.getmembers() if m.isfile()}

        def read_tar(name: str) -> bytes:
            stream = archive.extractfile(files[name])
            assert stream is not None
            return stream.read()

        return list(files), {n: m.mode & 0o777 for n, m in files.items()}, (archive, read_tar)
    archive = zipfile.ZipFile(path)
    return archive.namelist(), {}, (archive, archive.read)


def audit(path: Path) -> dict[str, object]:
    platform = infer_platform(path)
    if is_tarball(path) and platform == "windows":
        raise ValueError("Windows packages ship as a zip only")
    names, modes, (archive, read) = read_members(path)
    with archive:
        readme_name = root_member(names, "README-SETUP.txt")
        readme = read(readme_name).decode("utf-8")

        run_match = re.search(r"(?m)^\s*\d+\.\s+Run\s+([^\s.]+(?:\.exe)?)\.\s*$", readme)
        if not run_match:
            raise ValueError("README-SETUP.txt has no numbered Run instruction")
        host_name = run_match.group(1)
        if platform == "windows" and not host_name.lower().endswith(".exe"):
            host_name += ".exe"
        host_member = root_member(names, host_name)
        host_copy = read(host_member).decode("latin-1")
    if modes and modes[host_member] != 0o755:
        raise ValueError(f"setup host {host_member} is mode {modes[host_member]:o}, not 755")

    if platform == "windows":
        require(
            readme,
            ("downloads", "cmake-clang-v1", "toolchain pack", "toolchain folder of this game folder"),
            "Windows README-SETUP.txt",
        )
        # PS1B-687: the pack is installed into the game folder, not the user's
        # profile. A text that sends the player to AppData describes a route
        # the setup no longer takes (PSX-PUB-026).
        reject(readme.lower(), ("appdata", "localappdata"), "Windows README-SETUP.txt")
        require(
            host_copy,
            (
                "Download latest portable toolchain",
                "Select cmake-clang-v1 toolchain zip",
                "cmake-clang-v1",
            ),
            "Windows setup host",
        )
        reject(host_copy, ("%LOCALAPPDATA%\\retcomm\\toolchains",), "Windows setup host")
    else:
        require(
            readme,
            (
                "Install CMake, Ninja, Python 3",
                "C/C++ compiler",
                "available on PATH",
                "native build tools",
            ),
            "POSIX README-SETUP.txt",
        )
        reject(
            readme.lower(),
            ("cmake-clang-v1", "portable toolchain", "toolchain pack", "toolchain zip"),
            "POSIX README-SETUP.txt",
        )
        require(
            host_copy,
            (
                "1. Native build tools",
                "Install CMake, Ninja, Python 3, and either Clang or GCC",
                "Check tools",
            ),
            "POSIX setup host",
        )
        reject(
            host_copy,
            ("Download latest portable toolchain##tc", "1. Portable toolchain"),
            "POSIX setup host",
        )

    return {
        "archive": str(path.resolve()),
        "platform": platform,
        "sha256": sha256(path),
        "size": path.stat().st_size,
        "status": "pass",
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("archives", nargs="+", type=Path)
    parser.add_argument("--json", type=Path, dest="json_path")
    args = parser.parse_args()

    results: list[dict[str, object]] = []
    failed = False
    for path in args.archives:
        try:
            result = audit(path)
        except (OSError, ValueError, zipfile.BadZipFile, tarfile.TarError) as error:
            result = {"archive": str(path.resolve()), "status": "fail", "error": str(error)}
            failed = True
        results.append(result)
        print(f"{result['status'].upper()}: {path}: {result.get('error', result.get('platform'))}")

    if args.json_path:
        args.json_path.parent.mkdir(parents=True, exist_ok=True)
        args.json_path.write_text(
            json.dumps({"schema": 1, "results": results}, indent=2) + "\n",
            encoding="utf-8",
        )
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
