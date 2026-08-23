#!/usr/bin/env python3
"""Pin MDEC Command Busy to the complete input-plus-output transaction."""

from pathlib import Path
import sys


ROOT = Path(__file__).resolve().parents[2]


def main() -> int:
    source = (ROOT / "runtime/src/mdec.c").read_text(encoding="utf-8")
    required = (
        "if (mdec.busy || mdec.output_pos < mdec.output_size) "
        "status |= 1u << 29;"
    )
    if required not in source:
        raise AssertionError(
            "MDEC status bit 29 must stay busy until decoded output drains"
        )
    if "if (mdec.busy) status |= 1u << 29;" in source:
        raise AssertionError("input-only MDEC busy contract returned")
    print("PASS: MDEC Command Busy covers pending decoded output")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except AssertionError as exc:
        print(f"FAIL: {exc}")
        sys.exit(1)
