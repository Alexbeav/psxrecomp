"""The KSEG2 ignored-access counters are in the run report (PS1B-516).

The C fixture links the real memory.c and checks the report text for the two
counters. This driver builds it at -O0 and -O2, and then reads the one place
the fixture cannot link: crash_trace.c must print the field, and
docs/RUN_REPORT_KSEG2.md must describe it.
"""
import argparse
import tempfile
from pathlib import Path

import source_fixture_link
from source_fixture_link import build_and_run

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]

# memory.c compares bytes with memcmp, and Clang calls bcmp for a memcmp that
# is only tested for equality (see test_low_ram_game_entry.py).
source_fixture_link.ISO_C_FALLBACK |= {"memcmp", "bcmp"}


def check_wiring():
    report = (ROOT / "runtime/src/crash_trace.c").read_text(encoding="utf-8")
    assert "memory_kseg2_ignored_json(k2" in report and '\\"kseg2_ignored\\": ' in report, \
        "the run report must print kseg2_ignored"
    doc = (ROOT / "docs/RUN_REPORT_KSEG2.md").read_text(encoding="utf-8")
    assert "kseg2_ignored" in doc, "docs/RUN_REPORT_KSEG2.md must describe the field"


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--cc", default="gcc")
    args = parser.parse_args()
    check_wiring()
    with tempfile.TemporaryDirectory() as root:
        for opt in ("-O0", "-O2"):
            build_and_run(args.cc, HERE, HERE.parent, opt, Path(root),
                          ["memory.c", "crc32.c"], "test_kseg2_report_counters.c")
    print("PASS: the run report carries kseg2_ignored reads and writes (O0/O2)")
