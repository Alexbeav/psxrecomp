"""PS1B-306: every generated BIOS profile's kernel-bless table must fit the
runtime's verify-state array.

A table with more rows than PSX_KBLESS_MAX_ENTRIES cannot be blessed. The
runtime refuses to start on it and runtime.cmake fails the configure; this test
states the same rule over every generated <stem>_dispatch.c in the build tree,
with the row counts in its output. An old 4,096 cap was once exceeded without
anyone noticing (T110 grew the retail tables to 5,054-5,083 rows), which ran
every relocated kernel routine in the interpreter.

It also fails when a profile is close to the capacity (HEADROOM), so the next
table growth is seen before it becomes a refused build. It exits 77 (skipped)
when the build tree has no generated BIOS.
"""
import argparse
import re
import sys
from pathlib import Path

# Rows that must stay free. Measured 2026-10-01 on eeaadfd2a's emitter:
# SCPH1001 5054, SCPH5500 5083, SCPH5501 5054, SCPH5552 5054, OpenBIOS 3488,
# SCPH101 0 (no bless window).
HEADROOM = 2048

CAP = re.compile(r"#define\s+PSX_KBLESS_MAX_ENTRIES\s+(\d+)u?")
COUNT = re.compile(r"enum\s*\{\s*(\w+?)_psx_bios_kernel_body_count\s*=\s*(\d+)u?\s*\};")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--generated", required=True, type=Path)
    parser.add_argument("--capacity-header", required=True, type=Path)
    args = parser.parse_args()

    caps = CAP.findall(args.capacity_header.read_text(encoding="utf-8"))
    if len(caps) != 1:
        print(f"FAIL: expected one PSX_KBLESS_MAX_ENTRIES in {args.capacity_header}, found {len(caps)}")
        return 1
    cap = int(caps[0])

    found = {}
    for path in sorted(args.generated.glob("*_dispatch.c")) if args.generated.is_dir() else []:
        for stem, count in COUNT.findall(path.read_text(encoding="utf-8", errors="replace")):
            found[stem] = int(count)
    if not found:
        print(f"SKIP: no generated BIOS tables under {args.generated}")
        return 77

    failures = []
    for stem, count in sorted(found.items()):
        print(f"{stem}: {count} rows, capacity {cap}")
        if count > cap:
            failures.append(f"{stem} has {count} rows, more than the capacity {cap}; the runtime refuses it")
        elif count > cap - HEADROOM:
            failures.append(f"{stem} has {count} rows, within {HEADROOM} of the capacity {cap}; raise it")
    for failure in failures:
        print("FAIL:", failure)
    if not failures:
        print(f"PASS: {len(found)} profile(s) examined; every table fits with {HEADROOM} rows to spare")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
