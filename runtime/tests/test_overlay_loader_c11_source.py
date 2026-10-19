#!/usr/bin/env python3
"""Keep the overlay loader valid on the Linux release C11 compiler."""

from pathlib import Path
import re
import sys


def main() -> int:
    root = Path(__file__).resolve().parents[2]
    source = (root / "runtime/src/overlay_loader.c").read_text(encoding="utf-8")
    match = re.search(
        r"^retry_candidates:\s*\n(?P<body>(?:\s*.*\n){0,3})\s*int head = idx_head\(phys\);",
        source,
        re.MULTILINE,
    )
    if not match:
        raise AssertionError("overlay retry label or head declaration is missing")
    if not re.search(r"^\s*;", match.group("body"), re.MULTILINE):
        raise AssertionError("overlay retry label does not precede a C11 statement")

    print("overlay loader C11 source guard: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
