#!/usr/bin/env python3
"""Run the setup packager's private-path gate over the framework tree it stages.

tools/package_setup_host.sh copies this repository into a setup package with
tools/stage_framework_tree.sh and then stops the package when
tools/check_private_paths.sh finds a developer-machine path in it. Until this
test, that gate ran only inside a release build: a document line that named a
share folder passed every ctest on two hosts and then stopped every setup
package of the pin (PS1B-216).

This test stages the tree the same way and runs the same gate, so such a line
fails here. In a git checkout it stages the tracked files as they are in the
working tree; untracked litter is a developer's own business. Without git (a
source package) it stages the tree it is in.
"""

from __future__ import annotations

import os
from pathlib import Path
import shutil
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[2]
STAGER = ROOT / "tools" / "stage_framework_tree.sh"
GATE = ROOT / "tools" / "check_private_paths.sh"

# Files the staged tree must hold. A stage that lost them examined too little.
MUST_BE_STAGED = (
    "docs/beetle-linux.md",
    "README.md",
    "runtime/src/cdrom.c",
    "recompiler/CMakeLists.txt",
    "tools/package_setup_host.sh",
)
# Developer trees the stager drops. They may name workstation paths.
MUST_BE_DROPPED = ("docs/internal", "tools/tasreplays", "CLAUDE.md")


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
    raise AssertionError("bash is required for the staged-tree gate test")


def tracked_files() -> list[str] | None:
    """Tracked paths of this checkout, or None when there is no git here."""
    if not (ROOT / ".git").exists():
        return None
    result = subprocess.run(
        ["git", "-C", str(ROOT), "ls-files", "-z"],
        capture_output=True,
        check=False,
    )
    if result.returncode != 0:
        return None
    return [name for name in result.stdout.decode("utf-8").split("\0") if name]


def snapshot(names: list[str], dest: Path) -> int:
    copied = 0
    for name in names:
        source = ROOT / name
        if not source.is_file():      # deleted in the working tree, or a submodule
            continue
        target = dest / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, target)
        copied += 1
    return copied


def run(bash: str, *args: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [bash, *args],
        text=True,
        encoding="utf-8",
        errors="replace",
        capture_output=True,
        check=False,
    )


def main() -> None:
    bash = find_bash()
    with tempfile.TemporaryDirectory(prefix="psx-staged-tree-") as tmp:
        work = Path(tmp)
        names = tracked_files()
        if names is None:
            framework = ROOT
            print("no git checkout: staging the tree in place")
        else:
            framework = work / "src"
            copied = snapshot(names, framework)
            assert copied > 500, f"only {copied} tracked files were copied"
            print(f"snapshot: {copied} of {len(names)} tracked files")

        stage = work / "stage"
        staged = stage / "psxrecomp"
        result = run(bash, str(STAGER), "--framework", str(framework), "--dest", str(staged))
        assert result.returncode == 0, (result.returncode, result.stdout, result.stderr)

        count = sum(1 for path in staged.rglob("*") if path.is_file())
        for name in MUST_BE_STAGED:
            assert (staged / name).is_file(), f"the staged tree lacks {name}"
        for name in MUST_BE_DROPPED:
            assert not (staged / name).exists(), f"the staged tree still holds {name}"
        print(f"staged tree: {count} files")

        result = run(bash, str(GATE), str(stage))
        assert result.returncode == 0, (
            "the staged framework tree names a developer-machine path; "
            "the setup packager would refuse this tree:\n" + result.stderr
        )

        # The gate must be able to fail on this stage. A path is assembled
        # here so that this file does not itself hold one.
        probe = staged / "docs" / "gate-probe.md"
        probe.write_text("kept at " + "Z:" + "/Share/" + "probe/\n", encoding="utf-8")
        result = run(bash, str(GATE), str(stage))
        assert result.returncode == 1, (result.returncode, result.stdout, result.stderr)
        assert "psxrecomp/docs/gate-probe.md" in result.stderr.replace("\\", "/"), result.stderr

    print("setup staged tree private path test: PASS")


if __name__ == "__main__":
    main()
