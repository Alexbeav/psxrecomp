"""A build step that fails is built once more on its own (PS1B-342).

Under several concurrent builds clang once exited 1 with no diagnostic, and the
same compile passed when re-run by hand. In a parallel Ninja log the lines after
a FAILED step can be other steps' output, so a silent failure cannot be told
from a real one there. Each step Ninja reported FAILED is built again alone
(cmake --build <dir> --target <its output> --parallel 1): its output is then its
own. Used by psxrecomp_cli.py (rebuild) and the studio's Build.
"""
from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path
from typing import Callable

NINJA_FAILED = re.compile(r"^FAILED: (?:\[code=-?\d+\] )?(\S+)")
NINJA_NOISE = re.compile(r"^(\[\d+/\d+\]|ninja: )")
FAILED_STEPS_ALONE_MAX = 8


def failed_step_targets(output: str) -> list[str]:
    """The first output of each step ninja reports as FAILED, in order."""
    found: list[str] = []
    for line in output.splitlines():
        m = NINJA_FAILED.match(line)
        if m and m.group(1) not in found:
            found.append(m.group(1))
    return found


def host_build_load() -> str:
    """Free physical memory and the number of compiler and linker processes now (PS1B-342)."""
    names = {"clang", "clang++", "clang-cl", "cc1", "cc1plus", "gcc", "g++", "ld.lld", "lld",
             "lld-link", "ld", "ccache", "llvm-ar", "llvm-ranlib"}
    free_gb = None
    count = None
    try:
        if sys.platform == "win32":
            import ctypes

            class MEMORYSTATUSEX(ctypes.Structure):
                _fields_ = [("dwLength", ctypes.c_ulong), ("dwMemoryLoad", ctypes.c_ulong),
                            ("ullTotalPhys", ctypes.c_ulonglong), ("ullAvailPhys", ctypes.c_ulonglong),
                            ("ullTotalPageFile", ctypes.c_ulonglong), ("ullAvailPageFile", ctypes.c_ulonglong),
                            ("ullTotalVirtual", ctypes.c_ulonglong), ("ullAvailVirtual", ctypes.c_ulonglong),
                            ("ullAvailExtendedVirtual", ctypes.c_ulonglong)]
            st = MEMORYSTATUSEX()
            st.dwLength = ctypes.sizeof(st)
            if ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(st)):
                free_gb = st.ullAvailPhys / (1 << 30)
            out = subprocess.run(["tasklist", "/FO", "CSV", "/NH"], capture_output=True, text=True,
                                 encoding="utf-8", errors="replace", timeout=20)
            if out.returncode == 0:
                count = sum(1 for line in out.stdout.splitlines()
                            if line.startswith('"') and
                            line[1:].split('"', 1)[0].lower().removesuffix(".exe") in names)
        elif sys.platform.startswith("linux"):
            for line in Path("/proc/meminfo").read_text(encoding="utf-8").splitlines():
                if line.startswith("MemAvailable:"):
                    free_gb = int(line.split()[1]) / (1 << 20)
            count = 0
            for comm in Path("/proc").glob("[0-9]*/comm"):
                try:
                    if comm.read_text(encoding="utf-8").strip() in names:
                        count += 1
                except OSError:
                    continue
    except Exception:  # noqa: BLE001: a reading, never a reason to stop the build
        pass
    return "free memory %s, %s compiler or linker processes running" % (
        "%.1f GiB" % free_gb if free_gb is not None else "unknown",
        count if count is not None else "unknown")


def failed_steps_alone(base: list[str], returncode: int, output: str,
                       run: Callable[[list[str]], tuple[int, str]],
                       log: Callable[[str], None]) -> list[dict]:
    """PS1B-342. Builds each step the parallel build reported FAILED again on its
    own (cmake --build <dir> --target <its output> --parallel 1). Its output is
    then its own: in the parallel log the lines after a FAILED step can belong to
    other steps, so a step that printed nothing cannot be told apart there.
    Returns {target, exit, diagnostic} per step; empty when ninja named none or
    more than a handful (a real error in many files: nothing to learn here)."""
    targets = failed_step_targets(output)
    if not targets or len(targets) > FAILED_STEPS_ALONE_MAX:
        return []
    load = host_build_load()
    log("%d build step(s) failed (exit %d; %s); building each once more on its own"
        % (len(targets), returncode, load))
    results = []
    for target in targets:
        exit_code, said_text = run([*base, "--target", target, "--parallel", "1"])
        text = [line.strip() for line in said_text.splitlines()]
        said = [line for line in text if line and not NINJA_NOISE.match(line)
                and not NINJA_FAILED.match(line)]
        # The line after FAILED is the command; what follows it is the step's own output.
        if exit_code != 0 and said:
            said = said[1:]
        diagnostic = next((line for line in said if "error" in line.lower()), said[0] if said else "")
        results.append({"target": target, "exit": exit_code, "diagnostic": diagnostic[:300]})
        log("  %s on its own: exit %d%s" % (
            target, exit_code, "" if exit_code == 0 else
            ", " + (diagnostic[:300] if diagnostic else "no diagnostic")))
    return results
