#!/usr/bin/env python3
"""Find the host bash for this repository's tools and tests.

A Windows host can hold three programs named bash.exe. Git for Windows' bash
is an MSYS shell: it takes the Windows paths a test gives it. The other two
start WSL: %SystemRoot%\\System32\\bash.exe and the app alias in a
Microsoft\\WindowsApps folder. A WSL launcher hands the same arguments to a
Linux shell, which drops the backslashes, so the script is "not found" and the
test fails with exit 127 for no fault of the tree (PS1B-271). On a host whose
PATH puts System32 before Git, shutil.which("bash") is that launcher.

find_bash() never returns a launcher. It looks in the usual Git folders, then
beside a git.exe on PATH, then on PATH itself. PSX_GIT_BASH names the bash to
use when a host keeps Git somewhere else. When only launchers exist it says
so, and says what to install.
"""

from __future__ import annotations

import os
import shutil
import re
import subprocess


GIT_BASH_ENV = "PSX_GIT_BASH"
# Git for Windows ships its bash at both of these, relative to its root.
_GIT_BASH_RELATIVE = (("bin", "bash.exe"), ("usr", "bin", "bash.exe"))
# Where Windows is taken to be when the environment names neither SystemRoot
# nor windir. A caller that builds a small environment by hand can drop both,
# and the launcher in System32 is a launcher all the same. Only this one folder
# is ruled out: a rule on the name "system32" anywhere in a path could refuse a
# real Git bash.
DEFAULT_SYSTEM_ROOT = "C:\\Windows"


def _fold(path: str) -> str:
    return os.path.normcase(os.path.realpath(path)).casefold()


def is_wsl_launcher(path: str, environ=None) -> bool:
    """True for a bash.exe that starts WSL: any below %SystemRoot%, and the
    app alias, which Windows keeps in a folder named Microsoft\\WindowsApps."""
    environ = os.environ if environ is None else environ
    root = _fold(environ.get("SystemRoot") or environ.get("windir") or DEFAULT_SYSTEM_ROOT)
    # Keep the alias's original folder as well as its resolved target.
    for folded in (os.path.normcase(os.path.abspath(path)).casefold(), _fold(path)):
        if folded == root or folded.startswith(root.rstrip(os.sep) + os.sep):
            return True
        parts = folded.split(os.sep)
        if len(parts) >= 3 and parts[-2] == "windowsapps" and parts[-3] == "microsoft":
            return True
    return False


def windows_candidates(environ) -> list[str]:
    """Every place a bash.exe is looked for on Windows, best first."""
    path_dirs = [entry.strip('"') for entry in environ.get("PATH", "").split(os.pathsep)]
    path_dirs = [entry for entry in path_dirs if entry]

    roots = []
    for name in ("ProgramFiles", "ProgramW6432", "ProgramFiles(x86)"):
        if environ.get(name):
            roots.append(os.path.join(environ[name], "Git"))
    if environ.get("LOCALAPPDATA"):
        roots.append(os.path.join(environ["LOCALAPPDATA"], "Programs", "Git"))
    # A Git in another folder is found through its git.exe: <Git>\cmd from a
    # Windows shell, <Git>\mingw64\bin from inside Git Bash.
    for entry in path_dirs:
        if os.path.isfile(os.path.join(entry, "git.exe")):
            roots.append(os.path.dirname(entry))
            roots.append(os.path.dirname(os.path.dirname(entry)))

    candidates = [os.path.join(root, *relative)
                  for root in roots for relative in _GIT_BASH_RELATIVE]
    candidates += [os.path.join(entry, "bash.exe") for entry in path_dirs]

    seen = set()
    ordered = []
    for candidate in candidates:
        key = _fold(candidate)
        if key not in seen:
            seen.add(key)
            ordered.append(candidate)
    return ordered


def is_msys_bash(path: str, environ=None) -> bool:
    """Validate a non-launcher with the same MSYS checks as the replay tools."""
    if is_wsl_launcher(path, environ) or not os.path.isfile(path):
        return False
    for argv, marker in (([path, "-c", "echo $MSYSTEM"], r"MINGW|MSYS|UCRT|CLANG"),
                         ([path, "--version"], r"msys|mingw")):
        try:
            output = subprocess.run(argv, capture_output=True, text=True,
                                    encoding="utf-8", errors="replace", timeout=60,
                                    creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
                                    env=environ).stdout
        except (OSError, subprocess.SubprocessError):
            return False
        if re.search(marker, output or "", re.I):
            return True
    return False


def find_bash(purpose: str, environ=None, windows: bool | None = None,
              *, verify_msys: bool = False) -> str:
    """Choose one host shell. Windows launchers are never probed or returned.

    Production callers use verify_msys to retain the replay tools' MSYS check.
    The path-policy test uses authored empty files without starting a shell.
    """
    environ = os.environ if environ is None else environ
    windows = (os.name == "nt") if windows is None else windows
    explicit = environ.get(GIT_BASH_ENV)
    if explicit:
        if windows and is_wsl_launcher(explicit, environ):
            raise AssertionError(f"{GIT_BASH_ENV} names a WSL launcher: {explicit}")
        if not os.path.isfile(explicit):
            raise AssertionError(f"{GIT_BASH_ENV} names a file that does not exist: {explicit}")
        if windows and verify_msys and not is_msys_bash(explicit, environ):
            raise AssertionError(f"{GIT_BASH_ENV} is not a Git for Windows bash.exe: {explicit}")
        return explicit
    if not windows:
        found = shutil.which("bash", path=environ.get("PATH"))
        if found:
            return found
        raise AssertionError(f"bash is required for {purpose}; there is none on PATH")

    launchers = []
    for candidate in windows_candidates(environ):
        if is_wsl_launcher(candidate, environ):
            if os.path.lexists(candidate):
                launchers.append(candidate)
            continue
        if os.path.isfile(candidate) and (not verify_msys or is_msys_bash(candidate, environ)):
            return candidate
    if launchers:
        found = "The only bash.exe found starts WSL (" + ", ".join(launchers) + "). "
    else:
        found = "No suitable bash.exe was found. "
    raise AssertionError(f"Git for Windows bash is required for {purpose}. " + found
                         + f"Install Git for Windows, or set {GIT_BASH_ENV} to its bash.exe.")
