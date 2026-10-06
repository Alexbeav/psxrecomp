"""Exclusive file creation without the C runtime's exclusive-create flag (PS1B-206).

msvcrt.dll, the C runtime of an MSYS2 MINGW64 build, rejects the C11 "x" flag of
fopen: every such call returns NULL. The probes and the TAS checkpoint manifest
therefore create their files through runtime/include/psx_file_create_new.h.

Two checks:
1. test_file_create_new.c, built at O0 and O2 with -std=c11 -Werror and no
   feature macro, passes with the given compiler.
2. No C or C++ source of the runtime or the recompiler opens a stream with the
   "x" flag. UCRT and glibc accept the flag, so a new use would pass every other
   test on those hosts and fail only on msvcrt.
"""
import argparse
import os
import re
import subprocess
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
SOURCE_SUFFIXES = {".c", ".h", ".cpp", ".hpp", ".cc", ".inc"}
# Third-party code, build output and generated code are not ours to rule on.
SKIP_FOLDERS = {"lib", "generated", ".git", "__pycache__"}
OPEN_CALL = re.compile(r"\b(?:fopen|freopen|_wfopen|fopen_s|_wfopen_s)\s*\(")
STRING = re.compile(r'"((?:[^"\\\n]|\\.)*)"')
MODE = re.compile(r"[rwab+xtcn]{1,5}(?:,\s*ccs=[A-Za-z0-9-]+)?")


def skipped(name):
    lowered = name.lower()
    return (name in SKIP_FOLDERS or lowered.startswith("build")
            or "restricted" in lowered or "cleanroom" in lowered)


def sources():
    for top in ("runtime", "recompiler"):
        for folder, names, files in os.walk(ROOT / top):
            # Prune before descent: a skipped folder is never listed or read.
            names[:] = sorted(name for name in names if not skipped(name))
            for name in sorted(files):
                if Path(name).suffix.lower() in SOURCE_SUFFIXES:
                    yield Path(folder) / name


def exclusive_opens(text):
    """Line numbers of stream opens whose mode literal holds the "x" flag."""
    hits = []
    for call in OPEN_CALL.finditer(text):
        end = text.find(";", call.end())
        literals = STRING.findall(text[call.end():end if end >= 0 else call.end() + 400])
        if not literals:
            continue
        mode = literals[-1]
        if MODE.fullmatch(mode) and "x" in mode.split(",")[0]:
            hits.append(text.count("\n", 0, call.start()) + 1)
    return hits


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cc", default="gcc")
    compiler = parser.parse_args().cc

    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        for opt in ("-O0", "-O2"):
            exe = root / ("create" + opt + (".exe" if os.name == "nt" else ""))
            subprocess.run([compiler, "-std=c11", opt, "-Wall", "-Wextra", "-Werror",
                            "-I" + str(HERE.parent / "include"),
                            str(HERE / "test_file_create_new.c"), "-o", str(exe)], check=True)
            work = root / ("run" + opt)
            work.mkdir()
            result = subprocess.run([str(exe), str(work)], capture_output=True, timeout=30)
            assert result.returncode == 0, (opt, result.returncode, result.stdout, result.stderr)

    # The scanner itself: it sees the flag, and only the flag.
    assert exclusive_opens('f = fopen(path, "wbx");\n') == [1]
    assert exclusive_opens('f = fopen(path,\n          "wx");\n') == [1]
    assert exclusive_opens('f = fopen("x.txt", "w");\nf = fopen(name, mode);\n') == []

    found = []
    count = 0
    for path in sources():
        count += 1
        text = path.read_text(encoding="utf-8", errors="replace")
        found += [f"{path.relative_to(ROOT).as_posix()}:{line}" for line in exclusive_opens(text)]
    assert count > 100, f"only {count} source file(s) were read; the scan did not see the tree"
    assert not found, ("stream opened with the exclusive-create flag, which msvcrt rejects; "
                       "use psx_file_create_new(): " + ", ".join(found))
    print(f"PASS: psx_file_create_new at O0/O2; no exclusive-create fopen in {count} source files")


if __name__ == "__main__":
    main()
