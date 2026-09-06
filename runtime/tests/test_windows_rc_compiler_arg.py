#!/usr/bin/env python3
"""Keep the Windows resource compiler in the selected portable toolchain."""

import importlib.util
import sys
import tempfile
from pathlib import Path
from unittest import mock


ROOT = Path(__file__).resolve().parents[2]


def load_cli():
    spec = importlib.util.spec_from_file_location("psxrecomp_cli", ROOT / "psxrecomp_cli.py")
    assert spec and spec.loader
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main() -> None:
    cli = load_cli()
    with tempfile.TemporaryDirectory(prefix="PSX RC path with spaces ") as temp:
        bin_dir = Path(temp) / "toolchain bin"
        bin_dir.mkdir()
        compiler = bin_dir / "clang.exe"
        windres = bin_dir / "windres.exe"
        compiler.touch()
        windres.touch()
        with mock.patch.object(cli.sys, "platform", "win32"):
            args = cli._windows_rc_cmake_args(bin_dir, compiler)
        assert args == [f"-DCMAKE_RC_COMPILER={windres.resolve().as_posix()}"]
        assert "\\" not in args[0]
    print("windows RC compiler argument test: PASS")


if __name__ == "__main__":
    main()
