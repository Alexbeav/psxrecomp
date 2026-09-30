#!/usr/bin/env python3
"""Guard that the neGcon twist reads the host stick's X alone.

test_psx_stick_axis.c proves psx_stick_axis_to_byte depends on X only. This
proves the neGcon sampler in main.cpp actually takes the twist from it, not
from the radial DualShock transform, where moving the stick up or down moved
the twist. The sampler reads SDL devices in the middle of main.cpp and is not
reachable from a unit test, so the wiring is checked in the source, as
test_launcher_pad_mode_wiring.py does for the launcher.
"""

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MAIN = (ROOT / "runtime" / "src" / "main.cpp").read_text(encoding="utf-8")


def body(signature: str) -> str:
    """The text of the function that starts at `signature`, to its closing brace."""
    start = MAIN.index(signature)
    depth = 0
    for i in range(MAIN.index("{", start), len(MAIN)):
        if MAIN[i] == "{":
            depth += 1
        elif MAIN[i] == "}":
            depth -= 1
            if depth == 0:
                return MAIN[start : i + 1]
    raise AssertionError(f"unterminated body: {signature}")


helper = body("static uint8_t axis_to_pad_byte(")
assert "psx_stick_axis_to_byte(" in helper, "axis_to_pad_byte must use the one-axis transform"

sticks = body("static void pad_sticks_for(")
assert "bool left_x_alone" in sticks, "pad_sticks_for lost its left_x_alone flag"
assert re.search(r"if \(left_x_alone\) out\[0\] = axis_to_pad_byte\(lx, p\.deadzone\);", sticks), (
    "pad_sticks_for must replace out[0] with the one-axis X when left_x_alone is set"
)

sampler = body("static int sample_negcon_slot(")
calls = re.findall(r"pad_sticks_for\(([^;]*)\);", sampler)
assert calls, "sample_negcon_slot no longer reads the sticks"
for args in calls:
    assert args.replace(" ", "").endswith(",true"), (
        f"sample_negcon_slot reads the twist through the radial path: pad_sticks_for({args})"
    )

print("negcon twist wiring: ok")
