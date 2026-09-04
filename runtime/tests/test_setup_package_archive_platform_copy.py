#!/usr/bin/env python3
"""Exercise the exact setup-archive platform-copy release gate."""

from __future__ import annotations

import importlib.util
import tempfile
import zipfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
TOOL = ROOT / "tools" / "ci" / "audit_setup_package_platform_copy.py"
SPEC = importlib.util.spec_from_file_location("platform_copy_gate", TOOL)
assert SPEC and SPEC.loader
GATE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(GATE)

WINDOWS_README = """Setup\nPlatform: windows-x64\n1. Run Game.\nThe host downloads the cmake-clang-v1 toolchain pack.\n"""
POSIX_README = """Setup\nPlatform: {platform}\n1. Install CMake, Ninja, Python 3, and a C/C++ compiler.\n2. Make sure they are available on PATH.\n3. Run Game.\nThe host checks the native build tools.\n"""
WINDOWS_HOST = b"Download latest portable toolchain\0Select toolchain zip\0cmake-clang-v1"
POSIX_HOST = b"1. Native build tools\0Install CMake, Ninja, Python 3, and either Clang or GCC\0Check tools"


def write_zip(path: Path, readme: str, host_name: str, host: bytes) -> None:
    with zipfile.ZipFile(path, "w") as archive:
        archive.writestr("README-SETUP.txt", readme)
        archive.writestr(host_name, host)


def expect_fail(path: Path, message: str) -> None:
    try:
        GATE.audit(path)
    except ValueError:
        return
    raise AssertionError(message)


def main() -> None:
    with tempfile.TemporaryDirectory() as temp:
        root = Path(temp)
        windows = root / "game-1.0-windows-x64.zip"
        linux = root / "game-1.0-linux-x64.zip"
        macos = root / "game-1.0-macos-arm64.zip"
        stale = root / "stale-1.0-linux-x64.zip"
        stale_host = root / "stale-host-1.0-linux-x64.zip"
        weak_windows = root / "weak-1.0-windows-x64.zip"

        write_zip(windows, WINDOWS_README, "Game.exe", WINDOWS_HOST)
        write_zip(linux, POSIX_README.format(platform="linux-x64"), "Game", POSIX_HOST)
        write_zip(macos, POSIX_README.format(platform="macos-arm64"), "Game", POSIX_HOST)
        write_zip(stale, WINDOWS_README.replace("windows-x64", "linux-x64"), "Game", WINDOWS_HOST)
        write_zip(stale_host, POSIX_README.format(platform="linux-x64"), "Game", WINDOWS_HOST)
        write_zip(weak_windows, WINDOWS_README, "Game.exe", b"no setup controls")

        assert GATE.audit(windows)["platform"] == "windows"
        assert GATE.audit(linux)["platform"] == "linux"
        assert GATE.audit(macos)["platform"] == "macos"
        expect_fail(stale, "stale Windows copy passed a Linux archive")
        expect_fail(stale_host, "stale Windows controls passed a Linux archive")
        expect_fail(weak_windows, "Windows archive passed without portable-tool controls")

    print("setup package archive platform-copy test: PASS")


if __name__ == "__main__":
    main()
