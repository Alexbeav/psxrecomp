#!/usr/bin/env python3
"""Find the bash that runs this repository's shell scripts in a test.

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
    return os.path.normcase(os.path.abspath(path))


def is_wsl_launcher(path: str, environ=None) -> bool:
    """True for a bash.exe that starts WSL: any below %SystemRoot%, and the
    app alias, which Windows keeps in a folder named Microsoft\\WindowsApps."""
    environ = os.environ if environ is None else environ
    folded = _fold(path)
    root = _fold(environ.get("SystemRoot") or environ.get("windir") or DEFAULT_SYSTEM_ROOT)
    if folded == root or folded.startswith(root.rstrip(os.sep) + os.sep):
        return True
    parts = folded.split(os.sep)
    return (len(parts) >= 3
            and parts[-2] == os.path.normcase("WindowsApps")
            and parts[-3] == os.path.normcase("Microsoft"))


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


def find_bash(purpose: str, environ=None, windows: bool | None = None) -> str:
    """The bash to run a repository script with, for the test named `purpose`.

    `environ` and `windows` exist for the test of this function; a caller
    leaves them out.
    """
    environ = os.environ if environ is None else environ
    windows = (os.name == "nt") if windows is None else windows

    if not windows:
        found = shutil.which("bash", path=environ.get("PATH"))
        if found:
            return found
        raise AssertionError(f"bash is required for {purpose}; there is none on PATH")

    explicit = environ.get(GIT_BASH_ENV)
    if explicit:
        if is_wsl_launcher(explicit, environ):
            raise AssertionError(
                f"{GIT_BASH_ENV} names a WSL launcher, which cannot run the scripts "
                f"of {purpose}: {explicit}")
        if not os.path.isfile(explicit):
            raise AssertionError(f"{GIT_BASH_ENV} names a file that does not exist: {explicit}")
        return explicit

    launchers = []
    for candidate in windows_candidates(environ):
        if is_wsl_launcher(candidate, environ):
            if os.path.lexists(candidate):
                launchers.append(candidate)
            continue
        if os.path.isfile(candidate):
            return candidate

    if launchers:
        found = ("The only bash.exe found starts WSL (" + ", ".join(launchers) + "); "
                 "it runs a script in Linux with Windows paths and fails with exit 127. ")
    else:
        found = "No bash.exe was found. "
    raise AssertionError(
        f"Git for Windows bash is required for {purpose}. " + found
        + f"Install Git for Windows, or set {GIT_BASH_ENV} to its bash.exe.")
