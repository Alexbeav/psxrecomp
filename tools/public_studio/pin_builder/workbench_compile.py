"""What the build records when a compile fails, and when one may be tried again (PS1B-342).

On Eagle, 2026-10-01, one generated file's compile exited 1 with no output
while five to six builds shared the host; the same compile passed when run
again by hand. ccache counted it as the compiler's own failure. The cause is
not known. Two things follow:

  * Capture. When ninja prints `FAILED:`, record at once what could explain a
    silent failure: the exit code, the command's output, free memory, how many
    compilers and linkers run on the host, free disc space, and whether the
    object and its dependency file can be opened (locked or not).
  * One retry, in the caller (pin_build): only when every failed compile
    printed nothing. A compile that printed anything is a real error.

This module has no Workbench imports: pin_build imports it.
"""
import datetime
import os
from pathlib import Path
import re
import shutil
import time

FAILED = re.compile(r"^FAILED: (?:\[code=(-?\d+)\] )?(.+?)\s*$")
STATUS = re.compile(r"^\[\d+/\d+\] ")
COMPILERS = ("clang", "clang++", "clang-cl", "cc1", "cc1plus", "gcc", "g++", "cl")
LINKERS = ("lld-link", "ld.lld", "lld", "ld", "link")
RETRY_LIMIT = 3  # more silent failures than this in one build is not a stray event


def memory():
    """{'free_ram_gb', 'free_commit_gb'}; a value is None where the host cannot say."""
    if os.name == "nt":
        import ctypes
        from ctypes import wintypes

        class Status(ctypes.Structure):
            _fields_ = [("dwLength", wintypes.DWORD), ("dwMemoryLoad", wintypes.DWORD),
                        ("ullTotalPhys", ctypes.c_ulonglong), ("ullAvailPhys", ctypes.c_ulonglong),
                        ("ullTotalPageFile", ctypes.c_ulonglong), ("ullAvailPageFile", ctypes.c_ulonglong),
                        ("ullTotalVirtual", ctypes.c_ulonglong), ("ullAvailVirtual", ctypes.c_ulonglong),
                        ("ullAvailExtendedVirtual", ctypes.c_ulonglong)]

        status = Status()
        status.dwLength = ctypes.sizeof(Status)
        if ctypes.WinDLL("kernel32", use_last_error=True).GlobalMemoryStatusEx(ctypes.byref(status)):
            return {"free_ram_gb": round(status.ullAvailPhys / 1024 ** 3, 2),
                    "free_commit_gb": round(status.ullAvailPageFile / 1024 ** 3, 2)}
        return {"free_ram_gb": None, "free_commit_gb": None}
    try:
        free = os.sysconf("SC_AVPHYS_PAGES") * os.sysconf("SC_PAGE_SIZE") / 1024 ** 3
        return {"free_ram_gb": round(free, 2), "free_commit_gb": None}
    except (AttributeError, ValueError, OSError):
        return {"free_ram_gb": None, "free_commit_gb": None}


def process_names():
    """Lower-case executable names, without `.exe`, of every running process; None when unknown."""
    if os.name == "nt":
        import ctypes
        from ctypes import wintypes

        class Entry(ctypes.Structure):
            _fields_ = [("dwSize", wintypes.DWORD), ("cntUsage", wintypes.DWORD),
                        ("th32ProcessID", wintypes.DWORD), ("th32DefaultHeapID", ctypes.c_size_t),
                        ("th32ModuleID", wintypes.DWORD), ("cntThreads", wintypes.DWORD),
                        ("th32ParentProcessID", wintypes.DWORD), ("pcPriClassBase", ctypes.c_long),
                        ("dwFlags", wintypes.DWORD), ("szExeFile", ctypes.c_wchar * 260)]

        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel.CreateToolhelp32Snapshot.restype = wintypes.HANDLE
        kernel.CreateToolhelp32Snapshot.argtypes = [wintypes.DWORD, wintypes.DWORD]
        kernel.Process32FirstW.argtypes = kernel.Process32NextW.argtypes = [wintypes.HANDLE, ctypes.POINTER(Entry)]
        kernel.CloseHandle.argtypes = [wintypes.HANDLE]
        snapshot = kernel.CreateToolhelp32Snapshot(0x00000002, 0)  # TH32CS_SNAPPROCESS
        if snapshot in (None, wintypes.HANDLE(-1).value):
            return None
        names = []
        try:
            entry = Entry()
            entry.dwSize = ctypes.sizeof(Entry)
            more = kernel.Process32FirstW(snapshot, ctypes.byref(entry))
            while more:
                name = entry.szExeFile.lower()
                names.append(name[:-4] if name.endswith(".exe") else name)
                more = kernel.Process32NextW(snapshot, ctypes.byref(entry))
        finally:
            kernel.CloseHandle(snapshot)
        return names
    names = []
    try:
        for entry in Path("/proc").iterdir():
            if entry.name.isdigit():
                try:
                    names.append((entry / "comm").read_text().strip().lower())
                except OSError:
                    continue
    except OSError:
        return None
    return names


def lock_state(path):
    """{'exists', 'bytes', 'locked'} for a file the failed compile writes.

    `locked` is True when the file cannot be opened without sharing, which is
    what a compiler needs to replace it: another process has it open. None
    where the host cannot tell (not Windows) or the file does not exist.
    """
    path = Path(path)
    if not path.is_file():
        return {"exists": False, "bytes": None, "locked": None}
    state = {"exists": True, "bytes": path.stat().st_size, "locked": None}
    if os.name == "nt":
        import ctypes
        from ctypes import wintypes

        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel.CreateFileW.restype = wintypes.HANDLE
        kernel.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, wintypes.LPVOID,
                                       wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
        kernel.CloseHandle.argtypes = [wintypes.HANDLE]
        # GENERIC_READ, no sharing, OPEN_EXISTING. Nothing is written or truncated.
        handle = kernel.CreateFileW(str(path), 0x80000000, 0, None, 3, 0x80, None)
        if handle in (None, wintypes.HANDLE(-1).value):
            state["locked"] = ctypes.get_last_error() in (32, 33)  # sharing or lock violation
            state["open_error"] = ctypes.get_last_error()
        else:
            kernel.CloseHandle(handle)
            state["locked"] = False
    return state


def host_state(folder):
    """Memory, compiler load and disc space at this moment, for a failure record."""
    names = process_names()
    state = memory()
    state["compiler_processes"] = None if names is None else sum(name in COMPILERS for name in names)
    state["linker_processes"] = None if names is None else sum(name in LINKERS for name in names)
    try:
        probe = Path(folder)
        while not probe.exists() and probe != probe.parent:
            probe = probe.parent
        state["free_disk_gb"] = round(shutil.disk_usage(probe).free / 1024 ** 3, 1)
    except OSError:
        state["free_disk_gb"] = None
    return state


def depfile(command):
    """The dependency file a compile command writes (`-MF <path>`), or None."""
    found = re.search(r'(?:^|\s)-MF\s+("[^"]+"|\S+)', command)
    return found.group(1).strip('"') if found else None


class FailureWatch:
    """Fed every line of a ninja build; keeps one record per `FAILED:` block.

    Ninja prints `FAILED: [code=N] <outputs>`, then the command, then whatever
    the command printed, then goes on with the next `[n/m]` line. A block with
    no printed line is a silent failure.
    """

    def __init__(self, build_dir, state=host_state, lock=lock_state, clock=time.time):
        self.build_dir = Path(build_dir)
        self.failures = []
        self._state, self._lock, self._clock = state, lock, clock
        self._block, self._want_command, self._echo = None, False, False

    def line(self, text):
        started = FAILED.match(text)
        if started:
            self._close()
            stamp = datetime.datetime.fromtimestamp(self._clock()).isoformat(timespec="seconds")
            self._block = {"object": started[2], "exit_code": int(started[1]) if started[1] else None,
                           "at": stamp, "host": None, "output": []}
            self._want_command = True
            # The pin's CLI prints the build's output a second time when the build
            # ends (rebuild.log of run-4db439pd has every line twice). One ninja
            # run fails an object once, so a second block for it is that echo:
            # it is read to its end and not recorded.
            self._echo = any(seen["object"] == started[2] for seen in self.failures)
            if not self._echo:
                self._block["host"] = self._state(self.build_dir)
                self.failures.append(self._block)
            return
        if self._block is None:
            return
        if self._want_command:
            self._want_command = False
            self._block["command"] = text
            if self._echo:
                return
            files ={"object": self._lock(self.build_dir / self._block["object"])}
            dep = depfile(text)
            if dep:
                files["depfile"] = self._lock(self.build_dir / dep)
            self._block["files"] = files
            return
        if STATUS.match(text) or text.startswith("ninja: "):
            self._close()
            return
        self._block["output"].append(text)

    def _close(self):
        if self._block is not None:
            self._block["silent"] = not any(line.strip() for line in self._block["output"])
            self._block = None
            self._want_command = False

    def finish(self):
        """Close an open block and return the records."""
        self._close()
        return self.failures


def retry_reason(failures, pgo):
    """None when the failed build may be retried; otherwise why not, in plain words."""
    if not failures:
        return "the build did not fail in a compile step"
    if pgo:
        return "a PGO build runs its phases in one call and is not retried"
    loud = [f["object"] for f in failures if not f.get("silent")]
    if loud:
        return "a failed compile printed a diagnostic: " + ", ".join(loud[:3])
    if len(failures) > RETRY_LIMIT:
        return f"{len(failures)} compiles failed without output; more than {RETRY_LIMIT} is not a stray event"
    if any(" " in f["object"] for f in failures):
        return "a failed step has more than one output and cannot be run alone"
    return None


def gather_ccache_logs(debug_dir, target):
    """Join the per-file logs ccache wrote under `debug_dir` into one `.log` file. Returns its path or None.

    With CCACHE_DEBUG=1 and CCACHE_DEBUGDIR, ccache writes its full log for each
    compile below that folder. The build's evidence takes `*.log` files from the
    work folder, so the logs are joined into one file there.
    """
    debug_dir = Path(debug_dir)
    logs = sorted(p for p in debug_dir.rglob("*") if p.is_file() and "ccache-log" in p.name) if debug_dir.is_dir() else []
    if not logs:
        return None
    with Path(target).open("w", encoding="utf-8") as out:
        for log in logs:
            out.write(f"==== {log.relative_to(debug_dir).as_posix()}\n")
            out.write(log.read_text(encoding="utf-8", errors="replace"))
            out.write("\n")
    return Path(target)
