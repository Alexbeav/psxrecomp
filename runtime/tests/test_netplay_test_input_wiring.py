#!/usr/bin/env python3
"""Guard the netplay test-input switch (test tooling, PS1B-367 follow-up).

PSX_NET_TEST_INPUT_SEED scripts one peer's netplay pad so that a headless pair
mispredicts inputs and rolls back. It must do nothing unless it is set, must
never press Start or Select, must give one word per sim tick, and must sit at
the one place the local pad is staged. A match needs sockets and a second
peer, so this is checked in the source.
"""

from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]
MAIN = (ROOT / "runtime" / "src" / "main.cpp").read_text(encoding="utf-8")

signature = "static bool netplay_test_input(PsxNetPad* out) {"
start = MAIN.find(signature)
assert start >= 0, "main.cpp has no netplay_test_input"
depth, end = 0, None
for i in range(MAIN.index("{", start), len(MAIN)):
    if MAIN[i] == "{":
        depth += 1
    elif MAIN[i] == "}":
        depth -= 1
        if depth == 0:
            end = i + 1
            break
body = MAIN[start:end]

# Off unless the variable is set to a number above zero.
assert MAIN.count('"PSX_NET_TEST_INPUT_SEED"') == 1, "the variable must be read in one place"
assert 'std::getenv("PSX_NET_TEST_INPUT_SEED")' in body
assert body.index("if (!seed) return false;") < body.index("out->buttons"), (
    "with no seed the pad must be left alone"
)
# It says so when it is on.
assert "TEST INPUT" in body and "Test tooling only" in body, "a scripted pad must be announced"

# One word per sim tick: the tick and the seed are the only inputs.
assert "const uint32_t tick = psx_netplay_sim_tick();" in body and "(tick / 8u)" in body, (
    "the word must follow the session's sim tick"
)
# PSX_NET_TEST_INPUT_FROM only holds the pad idle before a tick; it cannot turn the script on.
assert MAIN.count('"PSX_NET_TEST_INPUT_FROM"') == 1 and 'std::getenv("PSX_NET_TEST_INPUT_FROM")' in body
assert "static uint32_t seed = 0, from = 0, hold = 0;" in body, "the default must be off, from the first tick"
# PSX_NET_TEST_INPUT_HOLD=1 holds Cross only; it cannot press Start or Select either.
assert MAIN.count('"PSX_NET_TEST_INPUT_HOLD"') == 1 and 'std::getenv("PSX_NET_TEST_INPUT_HOLD")' in body
assert "out->buttons = tick >= from ? (uint16_t)~(1u << 14) : 0xFFFFu;" in body, "the held button must be Cross alone"
assert "tick >= from &&" in body, "before the start tick the pad must be idle"
assert "static uint32_t frame" not in body and "++" not in body.split("if (!seed) return false;")[1], (
    "the word must not depend on how often it is sampled"
)

# Never Start (bit 3) or Select (bit 0): only bits 4-7 and 12-15 can be cleared.
held = re.search(r"held = \(uint16_t\)\(\(1u << \((\d+)u \+ \(x >> 28\) % 4u\)\) \|\s*\(1u << \((\d+)u \+ \(x >> 24\) % 4u\)\)\);", body)
assert held, "the held-button expression changed; review which bits it can clear"
assert (int(held.group(1)), int(held.group(2))) == (4, 12), "only the D-pad and the face buttons may be pressed"
assert "? (uint16_t)~held : 0xFFFFu" in body, "the pad word is active low: held bits cleared, the rest set"

# Consulted at the one place the local pad is staged, before the other sources.
calls = [m.start() for m in re.finditer(r"netplay_test_input\(&local\)", MAIN)]
assert len(calls) == 1, "the scripted pad must be consulted in exactly one place"
site = MAIN[calls[0] : calls[0] + 700]
assert site.index("netplay_test_input(&local)") < site.index("capture_override_pad(override, &local);") < site.index(
    "capture_local_human_pad(&local);"
) < site.index("psx_netplay_stage_local(&local);"), "the scripted pad must come first and still be staged"

print("netplay test input wiring test: PASS")
