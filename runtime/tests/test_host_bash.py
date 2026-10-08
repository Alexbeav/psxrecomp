#!/usr/bin/env python3
"""host_bash.find_bash() must never hand a test a WSL launcher (PS1B-271).

The cases use a made-up Windows layout in a temporary folder. No bash is
started, so the test runs the Windows rules on every host.
"""

from __future__ import annotations

import os
from pathlib import Path
import tempfile

import host_bash


PURPOSE = "the host bash test"


def touch(path: Path) -> str:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(b"")
    return str(path)


def refused(environ: dict) -> str:
    try:
        picked = host_bash.find_bash(PURPOSE, environ=environ, windows=True)
    except AssertionError as error:
        return str(error)
    raise AssertionError(f"find_bash returned {picked}; it had to refuse")


def main() -> None:
    cases = 0
    with tempfile.TemporaryDirectory(prefix="psx-host-bash-") as tmp:
        root = Path(tmp)
        system_root = root / "Windows"
        system32 = system_root / "System32"
        apps = root / "Home" / "AppData" / "Local" / "Microsoft" / "WindowsApps"
        wsl_system = touch(system32 / "bash.exe")
        wsl_alias = touch(apps / "bash.exe")
        program_files = root / "Program Files"
        git = program_files / "Git"
        git_bash = touch(git / "bin" / "bash.exe")
        touch(git / "usr" / "bin" / "bash.exe")
        touch(git / "cmd" / "git.exe")
        elsewhere = root / "Tools" / "PortableGit"
        elsewhere_bash = touch(elsewhere / "bin" / "bash.exe")
        touch(elsewhere / "cmd" / "git.exe")
        touch(elsewhere / "mingw64" / "bin" / "git.exe")
        msys_bin = root / "msys64" / "usr" / "bin"
        msys_bash = touch(msys_bin / "bash.exe")

        def env(path: list, **extra: str) -> dict:
            return {"SystemRoot": str(system_root),
                    "PATH": os.pathsep.join(str(entry) for entry in path), **extra}

        launchers_first = [system32, apps]

        # The two launchers are known as launchers; a Git bash is not.
        base = env(launchers_first)
        assert host_bash.is_wsl_launcher(wsl_system, base)
        assert host_bash.is_wsl_launcher(wsl_alias, base)
        assert not host_bash.is_wsl_launcher(git_bash, base)
        assert not host_bash.is_wsl_launcher(msys_bash, base)
        cases += 4

        # 1. The usual host: Git in Program Files, System32 first on PATH.
        picked = host_bash.find_bash(
            PURPOSE, environ=env(launchers_first + [git / "cmd"], ProgramFiles=str(program_files)),
            windows=True)
        assert picked == git_bash, picked
        cases += 1

        # 2. The host of the issue: no ProgramFiles in the environment, System32
        #    first on PATH. The old lookup returned the System32 launcher here.
        picked = host_bash.find_bash(PURPOSE, environ=env(launchers_first + [git / "cmd"]), windows=True)
        assert picked == git_bash, picked
        cases += 1

        # 3. Git in a folder of its own, found through git.exe on PATH: from a
        #    Windows shell (cmd) and from inside Git Bash (mingw64/bin).
        for git_dir in (elsewhere / "cmd", elsewhere / "mingw64" / "bin"):
            picked = host_bash.find_bash(PURPOSE, environ=env(launchers_first + [git_dir]), windows=True)
            assert picked == elsewhere_bash, (git_dir, picked)
            cases += 1

        # 4. No Git at all, another MSYS bash later on PATH: that one, not a launcher.
        picked = host_bash.find_bash(PURPOSE, environ=env(launchers_first + [msys_bin]), windows=True)
        assert picked == msys_bash, picked
        cases += 1

        # 5. Only launchers: refuse, name them, and say what to install.
        message = refused(env(launchers_first))
        assert wsl_system in message and wsl_alias in message, message
        assert "WSL" in message and "Git for Windows" in message, message
        assert host_bash.GIT_BASH_ENV in message, message
        cases += 1

        # 6. Nothing named bash.exe anywhere: refuse and say so.
        message = refused(env([root / "empty"]))
        assert "No bash.exe was found" in message and "Git for Windows" in message, message
        cases += 1

        # 7. PSX_GIT_BASH wins when it names a real bash, and is refused when it
        #    names a launcher or a missing file.
        picked = host_bash.find_bash(
            PURPOSE, environ=env(launchers_first + [git / "cmd"], PSX_GIT_BASH=msys_bash), windows=True)
        assert picked == msys_bash, picked
        message = refused(env(launchers_first + [git / "cmd"], PSX_GIT_BASH=wsl_system))
        assert "WSL launcher" in message and wsl_system in message, message
        message = refused(env(launchers_first + [git / "cmd"], PSX_GIT_BASH=str(root / "no" / "bash.exe")))
        assert "does not exist" in message, message
        cases += 3

        # 8. Windows spells SystemRoot in capitals on some hosts. Paths compare
        #    without case there; a case-sensitive host cannot run this case.
        if os.name == "nt":
            shouting = env(launchers_first)
            shouting["SystemRoot"] = str(system_root).upper()
            assert host_bash.is_wsl_launcher(wsl_system, shouting)
            assert wsl_system in refused(shouting)
            cases += 2

        # 9. Off Windows the bash on PATH is the bash.
        if os.name != "nt":
            assert host_bash.find_bash(PURPOSE)
            cases += 1

        # 10. An environment built by hand with neither SystemRoot nor windir.
        #     The old rule knew System32 only through those two names, so it
        #     returned the launcher here (review of PS1B-271). Windows is now
        #     taken to be at host_bash.DEFAULT_SYSTEM_ROOT.
        def bare(path: list) -> dict:
            return {"PATH": os.pathsep.join(str(entry) for entry in path)}

        default_root = host_bash.DEFAULT_SYSTEM_ROOT
        host_bash.DEFAULT_SYSTEM_ROOT = str(system_root)
        try:
            assert host_bash.is_wsl_launcher(wsl_system, bare(launchers_first))
            assert not host_bash.is_wsl_launcher(git_bash, bare(launchers_first))
            message = refused(bare(launchers_first))
            assert wsl_system in message and wsl_alias in message, message
            picked = host_bash.find_bash(
                PURPOSE, environ=bare(launchers_first + [git / "cmd"]), windows=True)
            assert picked == git_bash, picked
            picked = host_bash.find_bash(
                PURPOSE, environ=bare(launchers_first + [msys_bin]), windows=True)
            assert picked == msys_bash, picked
            cases += 5
        finally:
            host_bash.DEFAULT_SYSTEM_ROOT = default_root

        # 11. The folder that stands in is the usual one, and the rule is on
        #     that folder only: a Git kept below a folder that is merely named
        #     system32 is still a Git bash. A drive letter needs Windows.
        assert default_root == "C:\\Windows", default_root
        cases += 1
        if os.name == "nt":
            assert host_bash.is_wsl_launcher("C:\\Windows\\System32\\bash.exe", {})
            assert host_bash.is_wsl_launcher("c:\\windows\\system32\\BASH.EXE", {"PATH": ""})
            assert not host_bash.is_wsl_launcher("C:\\Program Files\\Git\\bin\\bash.exe", {})
            assert not host_bash.is_wsl_launcher("D:\\tools\\system32\\Git\\bin\\bash.exe", {})
            cases += 4

    # What this host itself gives a test. A host without Git Bash is reported
    # here and fails in the tests that need one, with the same sentence.
    try:
        chosen = host_bash.find_bash(PURPOSE)
    except AssertionError as error:
        chosen = None
        print(f"this host: no usable bash ({error})")
    else:
        assert os.name != "nt" or not host_bash.is_wsl_launcher(chosen), chosen
        print(f"this host: {chosen}")

    print(f"host bash test: PASS ({cases} cases)")


if __name__ == "__main__":
    main()
