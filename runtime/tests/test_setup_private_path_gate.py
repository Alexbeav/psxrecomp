#!/usr/bin/env python3
"""Exercise the staged-package private-path gate with deterministic fixtures."""

from __future__ import annotations

import os
from pathlib import Path
import shutil
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[2]
GATE = ROOT / "tools" / "check_private_paths.sh"


def find_bash() -> str:
    candidates = []
    program_files = os.environ.get("ProgramFiles")
    if program_files:
        candidates.append(Path(program_files) / "Git" / "bin" / "bash.exe")
    path_bash = shutil.which("bash")
    if path_bash:
        candidates.append(Path(path_bash))
    for candidate in candidates:
        if candidate.is_file():
            return str(candidate)
    raise AssertionError("bash is required for the package-gate regression")


def run_fixture(bash: str, value: str) -> subprocess.CompletedProcess[str]:
    with tempfile.TemporaryDirectory(prefix="psx-private-path-") as tmp:
        fixture = Path(tmp) / "fixture.txt"
        fixture.write_text(value + "\n", encoding="utf-8")
        return subprocess.run(
            [bash, str(GATE), tmp],
            text=True,
            capture_output=True,
            check=False,
        )


def run_stage(bash: str, stage: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [bash, str(GATE), str(stage)],
        text=True,
        capture_output=True,
        check=False,
    )


def stage_sdk_fixture(stage: Path) -> None:
    """Copy the exact SDK files that intentionally contain path examples."""
    sdk_files = (
        Path("README.md"),
        Path("docs/GAME_PROJECT_SETUP.md"),
        Path("runtime/tests/test_setup_private_path_gate.py"),
    )
    for relative in sdk_files:
        destination = stage / "psxrecomp" / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(ROOT / relative, destination)


def main() -> None:
    bash = find_bash()
    blocked = (
        r"L:\AgentData\codex-long-runs\receipt.txt",
        r"I:\OneDrive\PSX\notes.txt",
        r"Z:\Share\psxrecomp\private.txt",
        r"\\172.16.1.8\share\private.txt",
        "/mnt/i/Projects/private.txt",
        r"C:\Users\Alex\private.txt",
        "D:/Projects/private.txt",
    )
    allowed = (
        r"C:\Users\You\Games\disc.cue",
        r"C:\Users\username\Games\disc.cue",
        r"C:\Users\...\Games\disc.cue",
        "/home/user/public-example",
        "https://github.com/Alexbeav/project",
        r"I:\Games\public-example",
    )

    for value in blocked:
        result = run_fixture(bash, value)
        assert result.returncode == 1, (value, result.returncode, result.stdout, result.stderr)
        assert "developer-machine path" in result.stderr

    for value in allowed:
        result = run_fixture(bash, value)
        assert result.returncode == 0, (value, result.returncode, result.stdout, result.stderr)

    # Model the package's SDK stage. Known self-test paths and documented
    # placeholders are safe only in these exact SDK files. Real leakage in
    # executable, configuration, or package-output paths must still fail.
    with tempfile.TemporaryDirectory(prefix="psx-private-sdk-stage-") as tmp:
        stage = Path(tmp)
        stage_sdk_fixture(stage)
        result = run_stage(bash, stage)
        assert result.returncode == 0, (result.returncode, result.stdout, result.stderr)

        output_leaks = {
            Path("Silent_Hill__Recompiled.exe"): r"L:\AgentData\private\host.pdb",
            Path("game.toml"): r"Z:\Share\psxrecomp\owned\disc.cue",
            Path("package/receipt.json"): r"L:/AgentData/codex-long-runs/receipt.json",
        }
        for relative, value in output_leaks.items():
            destination = stage / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_text(value + "\n", encoding="utf-8")
            result = run_stage(bash, stage)
            assert result.returncode == 1, (
                relative,
                result.returncode,
                result.stdout,
                result.stderr,
            )
            assert str(relative).replace("\\", "/") in result.stderr.replace("\\", "/")
            destination.unlink()

    print("setup private path gate test: PASS")


if __name__ == "__main__":
    main()
